/*
OFDM modem encoder

Copyright 2021 Ahmet Inan <inan@aicodix.de>
*/

#include <iostream>
#include <cassert>
#include <cmath>
#include "xorshift.hh"
#include "complex.hh"
#include "utils.hh"
#include "bitman.hh"
#include "decibel.hh"
#include "fft.hh"
#include "wav.hh"
#include "pcm.hh"
#include "mls.hh"
#include "crc.hh"
#include "psk.hh"
#include "ldpc_tables.hh"
#include "ldpc_encoder.hh"
#include "galois_field.hh"
#include "bose_chaudhuri_hocquenghem_encoder.hh"

template <typename value, typename cmplx, int rate>
struct Encoder
{
	static const int symbol_len = (1280 * rate) / 8000;
	static const int guard_len = symbol_len / 8;
	static const int ldpc_bits = 64800;
	static const int bch_bits = ldpc_bits - 21600;
	static const int data_bits = bch_bits - 10 * 16;
	static const int mls0_len = 127;
	static const int mls0_poly = 0b10001001;
	static const int mls1_len = 255;
	static const int mls1_poly = 0b100101011;
	static const int mls2_poly = 0b100101010001;
	DSP::WritePCM<value> *pcm;
	DSP::FastFourierTransform<symbol_len, cmplx, 1> bwd;
	DSP::FastFourierTransform<4*symbol_len, cmplx, -1> fwd4;
	DSP::FastFourierTransform<4*symbol_len, cmplx, 1> bwd4;
	CODE::CRC<uint16_t> crc0;
	CODE::BoseChaudhuriHocquenghemEncoder<255, 71> bchenc0;
	CODE::BoseChaudhuriHocquenghemEncoder<65535, 65375> bchenc1;
	CODE::LDPCEncoder<DVB_T2_TABLE_A3> ldpcenc;
	int8_t code[ldpc_bits], bint[ldpc_bits];
	cmplx fdom[symbol_len], fdom4[4*symbol_len];
	cmplx tdom[symbol_len], tdom4[4*symbol_len];
	cmplx temp[symbol_len];
	cmplx guard[guard_len];
	cmplx papr_min, papr_max;
	int oper_mode;
	int mod_bits;
	int cons_cnt;
	int code_cols;
	int code_rows;
	int code_off;
	int mls0_off;
	int mls1_off;

	static int bin(int carrier)
	{
		return (carrier + symbol_len) % symbol_len;
	}
	static int bin4(int carrier)
	{
		return (carrier + 4*symbol_len) % (4*symbol_len);
	}
	static int nrz(bool bit)
	{
		return 1 - 2 * bit;
	}
	void improve_papr()
	{
		for (int i = 0; i < 4*symbol_len; ++i)
			fdom4[i] = 0;
		for (int i = -symbol_len/2; i < symbol_len/2; ++i)
			fdom4[bin4(i)] = fdom[bin(i)];
		bwd4(tdom4, fdom4);
		for (int i = 0; i < 4*symbol_len; ++i)
			tdom4[i] /= std::sqrt(value(4*symbol_len));
		for (int i = 0; i < 4*symbol_len; ++i) {
			value amp = std::max(std::abs(tdom4[i].real()), std::abs(tdom4[i].imag()));
			if (amp > value(1))
				tdom4[i] /= amp;
		}
		fwd4(fdom4, tdom4);
		for (int i = -symbol_len/2; i < symbol_len/2; ++i)
			if (norm(temp[bin(i)]))
				temp[bin(i)] = fdom4[bin4(i)] / std::sqrt(value(4*symbol_len));
			else
				temp[bin(i)] = 0;
	}
	void symbol(bool papr_reduction = true)
	{
		for (int i = 0; i < symbol_len; ++i)
			temp[i] = fdom[i];
		if (papr_reduction)
			improve_papr();
		bwd(tdom, temp);
		for (int i = 0; i < symbol_len; ++i)
			tdom[i] /= std::sqrt(value(8*symbol_len));
		for (int i = 0; i < guard_len; ++i) {
			value x = value(i) / value(guard_len - 1);
			x = value(0.5) * (value(1) - std::cos(DSP::Const<value>::Pi() * x));
			guard[i] = DSP::lerp(guard[i], tdom[i+symbol_len-guard_len], x);
		}
		cmplx peak, mean;
		for (int i = 0; i < symbol_len; ++i) {
			cmplx power(tdom[i].real() * tdom[i].real(), tdom[i].imag() * tdom[i].imag());
			peak = cmplx(std::max(peak.real(), power.real()), std::max(peak.imag(), power.imag()));
			mean += power;
		}
		if (mean.real() > 0 && mean.imag() > 0) {
			cmplx papr(peak.real() / mean.real(), peak.imag() / mean.imag());
			papr *= value(symbol_len);
			papr_min = cmplx(std::min(papr_min.real(), papr.real()), std::min(papr_min.imag(), papr.imag()));
			papr_max = cmplx(std::max(papr_max.real(), papr.real()), std::max(papr_max.imag(), papr.imag()));
		}
		pcm->write(reinterpret_cast<value *>(guard), guard_len, 2);
		pcm->write(reinterpret_cast<value *>(tdom), symbol_len, 2);
		for (int i = 0; i < guard_len; ++i)
			guard[i] = tdom[i];
	}
	void pilot_block()
	{
		CODE::MLS seq2(mls2_poly);
		value code_fac = std::sqrt(value(symbol_len) / value(code_cols));
		for (int i = 0; i < symbol_len; ++i)
			fdom[i] = 0;
		for (int i = code_off; i < code_off + code_cols; ++i)
			fdom[bin(i)] = code_fac * nrz(seq2());
		symbol();
	}
	void schmidl_cox()
	{
		CODE::MLS seq0(mls0_poly);
		value mls0_fac = std::sqrt(value(2 * symbol_len) / value(mls0_len));
		for (int i = 0; i < symbol_len; ++i)
			fdom[i] = 0;
		fdom[bin(mls0_off-2)] = mls0_fac;
		for (int i = 0; i < mls0_len; ++i)
			fdom[bin(2*i+mls0_off)] = nrz(seq0());
		for (int i = 0; i < mls0_len; ++i)
			fdom[bin(2*i+mls0_off)] *= fdom[bin(2*(i-1)+mls0_off)];
		symbol(false);
	}
	void meta_data(uint64_t md)
	{
		uint8_t data[9] = { 0 }, parity[23] = { 0 };
		for (int i = 0; i < 55; ++i)
			CODE::set_be_bit(data, i, (md>>i)&1);
		crc0.reset();
		uint16_t cs = crc0(md << 9);
		for (int i = 0; i < 16; ++i)
			CODE::set_be_bit(data, i+55, (cs>>i)&1);
		bchenc0(data, parity);
		CODE::MLS seq4(mls1_poly);
		value mls1_fac = std::sqrt(value(symbol_len) / value(mls1_len));
		for (int i = 0; i < symbol_len; ++i)
			fdom[i] = 0;
		fdom[bin(mls1_off-1)] = mls1_fac;
		for (int i = 0; i < 71; ++i)
			fdom[bin(i+mls1_off)] = nrz(CODE::get_be_bit(data, i));
		for (int i = 71; i < mls1_len; ++i)
			fdom[bin(i+mls1_off)] = nrz(CODE::get_be_bit(parity, i-71));
		for (int i = 0; i < mls1_len; ++i)
			fdom[bin(i+mls1_off)] *= fdom[bin(i-1+mls1_off)];
		for (int i = 0; i < mls1_len; ++i)
			fdom[bin(i+mls1_off)] *= nrz(seq4());
		symbol();
	}
	cmplx mod_map(int8_t *b)
	{
		switch (oper_mode) {
		case 2:
		case 3:
			return PhaseShiftKeying<8, cmplx, int8_t>::map(b);
		case 4:
		case 5:
			return PhaseShiftKeying<4, cmplx, int8_t>::map(b);
		}
		return 0;
	}
	void interleave()
	{
		for (int i = 0; i < cons_cnt; ++i)
			for (int k = 0; k < mod_bits; ++k)
				bint[mod_bits*i+k] = code[cons_cnt*k+i];
	}
	Encoder(DSP::WritePCM<value> *pcm, uint8_t *inp, int freq_off, uint64_t call_sign, int oper_mode) :
		pcm(pcm), crc0(0xA8F4), bchenc0({
			0b100011101, 0b101110111, 0b111110011, 0b101101001,
			0b110111101, 0b111100111, 0b100101011, 0b111010111,
			0b000010011, 0b101100101, 0b110001011, 0b101100011,
			0b100011011, 0b100111111, 0b110001101, 0b100101101,
			0b101011111, 0b111111001, 0b111000011, 0b100111001,
			0b110101001, 0b000011111, 0b110000111, 0b110110001}), bchenc1({
			0b10000000000101101, 0b10000000101110011, 0b10000111110111101,
			0b10101101001010101, 0b10001111100101111, 0b11111011110110101,
			0b11010111101100101, 0b10111001101100111, 0b10000111010100001,
			0b10111010110100111}),
			oper_mode(oper_mode)
	{
		switch (oper_mode) {
		case 2:
			code_cols = 432;
			mod_bits = 3;
			break;
		case 3:
			code_cols = 400;
			mod_bits = 3;
			break;
		case 4:
			code_cols = 400;
			mod_bits = 2;
			break;
		case 5:
			code_cols = 360;
			mod_bits = 2;
			break;
		default:
			return;
		}
		cons_cnt = ldpc_bits / mod_bits;
		code_rows = cons_cnt / code_cols;
		int offset = (freq_off * symbol_len) / rate;
		code_off = offset - code_cols / 2;
		mls0_off = offset - mls0_len + 1;
		mls1_off = offset - mls1_len / 2;
		papr_min = cmplx(1000, 1000), papr_max = cmplx(-1000, -1000);
		pilot_block();
		schmidl_cox();
		meta_data((call_sign << 8) | oper_mode);
		pilot_block();
		bchenc1(inp, inp+data_bits/8, data_bits);
		for (int i = 0; i < bch_bits; ++i)
			code[i] = nrz(CODE::get_le_bit(inp, i));
		ldpcenc(code, code + bch_bits);
		interleave();
		for (int j = 0; j < code_rows; ++j) {
			for (int i = 0; i < code_cols; ++i)
				fdom[bin(i+code_off)] *=
					mod_map(bint+mod_bits*(code_cols*j+i));
			symbol();
		}
		schmidl_cox();
		meta_data((call_sign << 8) | oper_mode);
		pilot_block();
		for (int i = 0; i < symbol_len; ++i)
			fdom[i] = 0;
		symbol();
		std::cerr << "real PAPR: " << DSP::decibel(papr_min.real()) << " .. " << DSP::decibel(papr_max.real()) << " dB" << std::endl;
		if (pcm->channels() == 2)
			std::cerr << "imag PAPR: " << DSP::decibel(papr_min.imag()) << " .. " << DSP::decibel(papr_max.imag()) << " dB" << std::endl;
	}
};

long long int base37_encoder(const char *str)
{
	long long int acc = 0;
	for (char c = *str++; c; c = *str++) {
		acc *= 37;
		if (c >= '0' && c <= '9')
			acc += c - '0' + 1;
		else if (c >= 'a' && c <= 'z')
			acc += c - 'a' + 11;
		else if (c >= 'A' && c <= 'Z')
			acc += c - 'A' + 11;
		else if (c != ' ')
			return -1;
	}
	return acc;
}

int main(int argc, char **argv)
{
	if (argc < 6 || argc > 9) {
		std::cerr << "usage: " << argv[0] << " OUTPUT RATE BITS CHANNELS INPUT [OFFSET] [CALLSIGN] [MODE]" << std::endl;
		return 1;
	}

	const char *output_name = argv[1];
	int output_rate = std::atoi(argv[2]);
	int output_bits = std::atoi(argv[3]);
	int output_chan = std::atoi(argv[4]);
	const char *input_name = argv[5];

	int freq_off = output_chan == 1 ? 2000 : 0;
	if (argc >= 7)
		freq_off = std::atoi(argv[6]);

	long long int call_sign = base37_encoder("ANONYMOUS");
	if (argc >= 8)
		call_sign = base37_encoder(argv[7]);

	if (call_sign <= 0 || call_sign >= 129961739795077L) {
		std::cerr << "Unsupported call sign." << std::endl;
		return 1;
	}

	int oper_mode = 2;
	if (argc >= 9)
		oper_mode = std::atoi(argv[8]);
	if (oper_mode < 2 || oper_mode > 5) {
		std::cerr << "Unsupported operation mode." << std::endl;
		return 1;
	}

	int band_width;
	switch (oper_mode) {
	case 2:
		band_width = 2700;
		break;
	case 3:
	case 4:
		band_width = 2500;
		break;
	case 5:
		band_width = 2250;
		break;
	default:
		return 1;
	}

	if ((output_chan == 1 && freq_off < band_width / 2) || freq_off < band_width / 2 - output_rate / 2 || freq_off > output_rate / 2 - band_width / 2) {
		std::cerr << "Unsupported frequency offset." << std::endl;
		return 1;
	}

	if (freq_off % 50) {
		std::cerr << "Frequency offset must be divisible by 50." << std::endl;
		return 1;
	}

	typedef float value;
	typedef DSP::Complex<value> cmplx;

	std::ifstream input_file(input_name, std::ios::binary);
	if (input_file.bad()) {
		std::cerr << "Couldn't open file \"" << input_name << "\" for reading." << std::endl;
		return 1;
	}
	const int code_len = 64800 / 8;
	const int data_len = code_len - (10 * 16 + 21600) / 8;
	uint8_t *input_data = new uint8_t[code_len];
	for (int i = 0; i < data_len; ++i)
		input_data[i] = input_file.get();
	CODE::Xorshift32 scrambler;
	for (int i = 0; i < data_len; ++i)
		input_data[i] ^= scrambler();

	DSP::WriteWAV<value> output_file(output_name, output_rate, output_bits, output_chan);
	output_file.silence(output_rate);
	switch (output_rate) {
	case 8000:
		delete new Encoder<value, cmplx, 8000>(&output_file, input_data, freq_off, call_sign, oper_mode);
		break;
	case 16000:
		delete new Encoder<value, cmplx, 16000>(&output_file, input_data, freq_off, call_sign, oper_mode);
		break;
	case 44100:
		delete new Encoder<value, cmplx, 44100>(&output_file, input_data, freq_off, call_sign, oper_mode);
		break;
	case 48000:
		delete new Encoder<value, cmplx, 48000>(&output_file, input_data, freq_off, call_sign, oper_mode);
		break;
	default:
		std::cerr << "Unsupported sample rate." << std::endl;
		return 1;
	}
	output_file.silence(output_rate);
	delete []input_data;

	return 0;
}

