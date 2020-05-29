#include "pch.h"
#include "lime\LimeSuite.h"
#include <string>
#include <windowsx.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>

#define TX_BUFFER_SIZE 2 * 1024 * 16

#define CP 0x7fff

using namespace std;

typedef struct {
	short re;
	short im;
}scmplx;

scmplx m_qpsk[4];
scmplx m_qpsk_2[4];
#define LIME_FN 128
int16_t m_hb_filter_c[LIME_FN];
scmplx m_filb[640000];
static short *m_filter = NULL;

unsigned char input_packet[125000];
unsigned char final_packet[156250];

lms_stream_t tx_stream;
lms_stream_meta_t meta_tx;
static scmplx  m_samples[100000];

static lms_info_str_t dev_list[255];
int deviceCount, comboboxIdx;

void lime_tx_samples(scmplx *s, int len) {
	LMS_SendStream(&tx_stream, s, len, &meta_tx, 1000000);
}

//
// Create a single oversampled Root raised cosine filter
//
void build_rrc_filter(float *filter, float rolloff, int ntaps, int samples_per_symbol) {
	double a, b, c, d;
	double B = rolloff + 0.0001;// Rolloff factor .0001 stops infinite filter coefficient with 0.25 rolloff
	double t = -(ntaps - 1) / 2;// First tap
	double Ts = samples_per_symbol;// Samples per symbol
	// Create the filter
	for (int i = 0; i < (ntaps); i++) {
		a = 2.0 * B / (M_PI*sqrt(Ts));
		b = cos((1.0 + B)*M_PI*t / Ts);
		// Check for infinity in calculation (computers don't have infinite precision)
		if (t == 0)
			c = (1.0 - B)*M_PI / (4 * B);
		else
			c = sin((1.0 - B)*M_PI*t / Ts) / (4.0*B*t / Ts);

		d = (1.0 - (4.0*B*t / Ts)*(4.0*B*t / Ts));
		//filter[i] = (b+c)/(a*d);//beardy
		filter[i] = (float)(a*(b + c) / d);//nasa
		t = t + 1.0;
	}
}

//
// Set the overall gain of the filter
//
void set_filter_gain(float *c, int len, float gain) {
	float a = 0;
	for (int i = 0; i < len; i++) {
		a += c[i];
	}
	a = gain / fabsf(a);
	for (int i = 0; i < len; i++) {
		c[i] = c[i] * a;
	}
}

void make_short(short *out, float *in, int len) {
	for (int i = 0; i < len; i++) {
		out[i] = (short)(in[i] * 32768);
	}
}

void window_filter(float *filter, int N) {
	// Build the window
	for (int i = -N / 2, j = 0; i < N / 2; i++, j++)
	{
		filter[j] = (0.5f*(1.0f + cosf((2.0f*(float)M_PI*i) / N)))*filter[j];
	}
}

short *rrc_make_filter(float roff, float mag, int ratio, int taps) {
	// Create the over sampled mother filter
	float *filter = (float*)malloc(sizeof(float)*taps);
	// Set last coefficient to zero
	filter[taps - 1] = 0;
	// RRC filter must always be odd length
	build_rrc_filter(filter, roff, taps - 1, ratio);
	window_filter(filter, taps - 1);
	set_filter_gain(filter, mag, taps);
	// Free memory from last filter if it exsists
	if (m_filter != NULL) free(m_filter);
	// Allocate memory for new global filter
	m_filter = (short*)malloc(sizeof(short)*taps);
	// convert the filter into shorts
	make_short(m_filter, filter, taps);
	free(filter);
	return m_filter;
}

int lime_rrc_interpolate_x2(scmplx *sin, scmplx *sout, int len) {
	// Copy in new samples
	memcpy(&m_filb[LIME_FN / 2], sin, sizeof(scmplx)*len);
	int32_t re, im;
	int idx = 0;
	for (int i = 0; i < len; i++) {
		re = im = 0;
		re = m_filb[i].re * m_hb_filter_c[1];
		im = m_filb[i].im * m_hb_filter_c[1];
		for (int j = 1; j < LIME_FN / 2; j++) {
			re += m_filb[i + j].re * m_hb_filter_c[(j * 2) + 1];
			im += m_filb[i + j].im * m_hb_filter_c[(j * 2) + 1];
		}
		sout[idx].re = re >> 15;
		sout[idx].im = im >> 15;
		idx++;
		re = m_filb[i].re * m_hb_filter_c[0];
		im = m_filb[i].im * m_hb_filter_c[0];
		for (int j = 1; j < LIME_FN / 2; j++) {
			re += m_filb[i + j].re * m_hb_filter_c[(j * 2)];
			im += m_filb[i + j].im * m_hb_filter_c[(j * 2)];
		}
		sout[idx].re = re >> 15;
		sout[idx].im = im >> 15;
		idx++;
	}
	// Save tail for next run of filter
	memcpy(m_filb, &sin[len - LIME_FN / 2], sizeof(scmplx)*LIME_FN / 2);
	// return new length
	return (len * 2);
}

void lime_build_x2_interpolator(float roff) {
	short *fir = rrc_make_filter(roff, 1.0, 2, LIME_FN);
	memcpy(m_hb_filter_c, fir, sizeof(int16_t)*LIME_FN);
}

void lime_transmit(scmplx *s, int len) {
	static int offset;
	len = lime_rrc_interpolate_x2(s, &m_samples[offset], len);
	offset += len;
	if (offset > 20000) {
		lime_tx_samples(m_samples, offset);
		offset = 0;
	}
}

int main(int argc, const char* argv[])
{

	bool verbose = false;
	bool verboseUnderrunOnly = false;
	float gain = 1.0f;
	string arg, param, inputFilename, deviceStringArg;
	inputFilename = "";
	deviceStringArg = "";
	unsigned int channel = 1;
	for (int i = 1; i < argc; i++) {
		arg = argv[i];
		if (arg.compare("-f") == 0) {
			if ((i + 1) < argc) {
				inputFilename = argv[i + 1];
				i++;
				continue;
			}
			else {
				cout << "Error: no input filename";
				return -1;
			}
		}
		if (arg.compare("-ch") == 0) {
			if ((i + 1) < argc) {
				channel = atoi(argv[i + 1]);
				i++;
				continue;
			}
			else {
				cout << "Error: no channel was specified";
				return -1;
			}
		}
		if ((arg.compare("-verbose") == 0) || (arg.compare("-v") == 0)) {
			verbose = true;
			continue;
		}
		if ((arg.compare("-verboseunderrun") == 0) || (arg.compare("-vu") == 0)) {
			verboseUnderrunOnly = true;
			continue;
		}
		if ((arg.compare("-list") == 0) || (arg.compare("-l") == 0)) {
			deviceCount = LMS_GetDeviceList(dev_list);
			if (deviceCount < 1) {
				cout << "No Lime devices found.";
				return -1;
			}
			cout << deviceCount << " Lime device" + string(deviceCount > 1 ? "s" : "") + " found" << endl;
			for (int i = 0; i < deviceCount; i++) {
				cout << dev_list[i] << endl;
			}
			return 0;
		}
		if ((arg.compare("-device") == 0) || (arg.compare("-d") == 0)) {
			if ((i + 1) < argc) {
				deviceStringArg = argv[i + 1];
				i++;
				continue;
			}
			else {
				cout << "Error: no device string was specified";
				return -1;
			}
		}
		if ((arg.compare("-gain") == 0) || (arg.compare("-g") == 0)) {
			if ((i + 1) < argc) {
				gain = atof(argv[i + 1]);
				i++;
				continue;
			}
			else {
				cout << "Error: no gain was specified";
				return -1;
			}
		}
		if ((arg.compare("-help") == 0) || (arg.compare("-h") == 0) || (arg.compare("-?") == 0) || (arg.compare("/?") == 0)) {
			cout << "Argument list" << endl << endl;
			cout << "-f [file to broadcast]" << endl;
			cout << "-ch [channel to broadcast on (1-30 inclusive), default is 1]" << endl;
			cout << "-v or -verbose (shows the buffer status), default is false" << endl;
			cout << "-vu or -verboseunderrun (shows the buffer status only for underruns), default is false" << endl;
			cout << "-l or -list (lists all connected Lime devices)" << endl;
			cout << "-d or -device [device string (from the list provided by -l or -list), defaults to the first device]" << endl;
			cout << "-g or -gain [transmit gain (0.01-1.0 inclusive), default 1.0]" << endl;
			cout << "-h or -? or /? or -help (shows this help message)" << endl;
			return 0;
		}
	}

	if (inputFilename.empty()) {
		cout << "A filename is required.";
		return -1;
	}

	if (channel < 1) {
		channel = 1;
	}
	else if (channel > 30) {
		channel = 30;
	}

	if (gain < 0.01) {
		gain = 0.01f;
	}
	else if (gain > 1.0f) {
		gain = 1.0f;
	}

	cout << "UTSC transmitter starting" << endl;
	cout << "Input file: " << inputFilename << endl;
	cout << "Channel: " << channel << endl << endl;

	double r0, r1;
	r0 = 1;
	r1 = 1;
	m_qpsk[0].re = (short)((r1*cos(M_PI / 4.0))*CP);
	m_qpsk[0].im = (short)((r1*sin(M_PI / 4.0))*CP);
	m_qpsk[1].re = (short)((r1*cos(7 * M_PI / 4.0))*CP);
	m_qpsk[1].im = (short)((r1*sin(7 * M_PI / 4.0))*CP);
	m_qpsk[2].re = (short)((r1*cos(3 * M_PI / 4.0))*CP);
	m_qpsk[2].im = (short)((r1*sin(3 * M_PI / 4.0))*CP);
	m_qpsk[3].re = (short)((r1*cos(5 * M_PI / 4.0))*CP);
	m_qpsk[3].im = (short)((r1*sin(5 * M_PI / 4.0))*CP);

	m_qpsk_2[0].re = (short)((r1*cos(0))*CP);
	m_qpsk_2[0].im = (short)((r1*sin(0))*CP);
	m_qpsk_2[1].re = (short)((r1*cos(3 * M_PI / 2.0))*CP);
	m_qpsk_2[1].im = (short)((r1*sin(3 * M_PI / 2.0))*CP);
	m_qpsk_2[2].re = (short)((r1*cos(M_PI / 2.0))*CP);
	m_qpsk_2[2].im = (short)((r1*sin(M_PI / 2.0))*CP);
	m_qpsk_2[3].re = (short)((r1*cos(M_PI))*CP);
	m_qpsk_2[3].im = (short)((r1*sin(M_PI))*CP);

	lms_info_str_t *deviceString;

	if (deviceStringArg.empty()) {
		deviceCount = LMS_GetDeviceList(dev_list);
		if (deviceCount == -1) return -1;
		deviceString = &(dev_list[0]);
	}
	else {
		if (verbose) cout << endl << "Using device string from cmdline" << endl;
		deviceString = (lms_info_str_t*)deviceStringArg.c_str();
	}
	
	cout << "Selected device: " << *deviceString << endl;

	lms_device_t* device;
	if (LMS_Open(&device, *deviceString, NULL) != 0) return -1;
	if (LMS_Init(device) != 0) return -1;
	if (LMS_EnableChannel(device, LMS_CH_TX, 0, true) != 0) return -1;
	if (LMS_SetSampleRate(device, 625000 * 2, 16) != 0) return -1;
	if (LMS_SetLOFrequency(device, LMS_CH_TX, 0, ((channel - 1) * 843750) + 902421875) != 0) return -1;
	if (LMS_SetAntenna(device, LMS_CH_TX, 0, LMS_PATH_TX1) != 0) return -1;
	//if (LMS_SetGaindB(device, LMS_CH_TX, 0, 73) != 0) return;
	if (LMS_SetNormalizedGain(device, LMS_CH_TX, 0, gain) != 0) return -1;
	if (LMS_SetLPFBW(device, LMS_CH_TX, 0, 5e6) != 0) return -1;
	if (LMS_SetLPF(device, LMS_CH_TX, 0, true) != 0) return -1;
	LMS_Calibrate(device, LMS_CH_TX, 0, 10e6, 0);
	lime_build_x2_interpolator(0.35f);

	tx_stream.channel = 0;
	tx_stream.fifoSize = 10485760;
	tx_stream.throughputVsLatency = 1.0f;
	tx_stream.dataFmt = lms_stream_t::LMS_FMT_I16;
	tx_stream.isTx = true;

	meta_tx.waitForTimestamp = false;
	meta_tx.flushPartialPacket = false;
	meta_tx.timestamp = 0;
	scmplx tx_buffer[TX_BUFFER_SIZE];

	char* fileData = new char[TX_BUFFER_SIZE / 4];
	std::ifstream file(inputFilename, std::ios::binary);

	LMS_SetupStream(device, &tx_stream);
	LMS_StartStream(&tx_stream);
	lms_stream_status_t status;
	while (true) {
		LMS_GetStreamStatus(&tx_stream, &status);
		if (status.fifoFilledCount < (status.fifoSize - TX_BUFFER_SIZE)) {
			if (verbose || (verboseUnderrunOnly && (status.underrun != 0))) cout << "\nSamples in FIFO: " << status.fifoFilledCount << ", overrun=" << status.overrun << ", underrun=" << status.underrun << flush;
			file.read(fileData, TX_BUFFER_SIZE / 4);
			if (file.gcount() < TX_BUFFER_SIZE / 4) break;
			unsigned int tmp_idx = 0;
			for (int i = 0; i < (TX_BUFFER_SIZE / 4); i++) {
				for (int j = 6; j >= 0; j -= 2) {
					tx_buffer[tmp_idx++] = (tmp_idx % 2 == 0) ? m_qpsk[(fileData[i] >> j) & 0x3] : m_qpsk_2[(fileData[i] >> j) & 0x3];
				}
			}
			lime_transmit(tx_buffer, TX_BUFFER_SIZE);
		}
	}

	file.close();
	delete[] fileData;

	LMS_StopStream(&tx_stream);
	LMS_DestroyStream(device, &tx_stream);

	LMS_Close(device);

	return 0;
}