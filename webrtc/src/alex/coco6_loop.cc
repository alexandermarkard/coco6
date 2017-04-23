// -----------------------------------------------------------------------
// Windows Includes
// -----------------------------------------------------------------------

#include <windows.h>
#include <winsock2.h>


// -----------------------------------------------------------------------
// Standard Librarys
// -----------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


// -----------------------------------------------------------------------
// WebRTC Librarys
// -----------------------------------------------------------------------

#include "webrtc/modules/audio_device/include/audio_device.h"
#include "webrtc/modules/audio_device/win/audio_device_core_win.h"
#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"

#include "third_party/opus/src/include/opus.h"

typedef struct {
	unsigned char minimax[6];
	unsigned char length;
	unsigned char opus[250];
} IOUTPACKET, *POUTPACKET;

typedef struct {
	unsigned char ID;
	unsigned char length;
	unsigned char opus[250];
} IPACKET, *PPACKET;

double prc( double a, double b, double dist )
{
	double out;
	double diff;
	
	diff = b - a;
	out = a + (diff*dist);
	
	return out;
}

void rsmpl( float *inpcm, float *outpcm, int inln, int outln )
{
	float q;
	q = (float)inln / (float)outln;
	
	int i;
	i = 0;
	int n;
	n = 0;
	int y;
	float x;
	x = 0.0;
	
	float a;
	float b;
	
	for( i = 0; i < outln; i++ )
	{
		a = inpcm[n];
		y = n + 1;
		if( y >= inln )
			y--;
		b = inpcm[y];
		outpcm[i] = prc( a, b, x );
		x += q;
		while( x > 1.0 )
		{
			x -= 1.0;
			n++;
		}
	}
}

void fade( short *pcm, bool in, bool out )
{
	int i;
	float v;
	float vinc;
	float f;

	v = 0.0f;
	vinc = 1.0f / 480.0f;
	for( i = 0; i < 480; i++ )
	{
		if( in )
		{
			f = (float)pcm[i] / 32768.0;
			f *= v;
			pcm[i] = (short)(f * 32766.0);
		}
		
		if( out )
		{
			f = (float)pcm[479 - i] / 32768.0;
			f *= v;
			pcm[479 - i] = (short)(f * 32766.0);
		}
		
		v += vinc;
	}
}


webrtc::AudioProcessing *apm;
SOCKET sock;
SOCKADDR_IN addr;
WSADATA wsa;

OpusDecoder *client_dec[30];
bool client_online[30];
int client_position[30];
int client_fec[30];

class AudioTransportImpl: public webrtc::AudioTransport
{
	private:
	short ultrabuffer_input[2000000];
	
	float nmul;
	float hi;
	float ohi;
	
	OpusEncoder *enc;
	IOUTPACKET outpacket;
	
	public:
	AudioTransportImpl()
	{
		int i;

		enc = opus_encoder_create( 48000, 1, OPUS_APPLICATION_VOIP, &i );
		opus_encoder_ctl( enc, OPUS_SET_BITRATE(80000) );
		opus_encoder_ctl( enc, OPUS_SET_COMPLEXITY(10) );
		opus_encoder_ctl( enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE) );

		for( i = 0; i < 2000000; i++ ) {
			ultrabuffer_input[i] = 0;
		}

		nmul = 1.0;
		hi = 0.0;
		ohi = 0.0;
	}
	
	~AudioTransportImpl()
	{
	}
	
	void put( short *pcm, int pos )
	{
		int y;
		y = pos * 48;
		int i;
		int x;
		for( i = 0; i < 480; i++ )
		{
			x = (int)ultrabuffer_input[i + y];
			x += (int)pcm[i];
			
			if( x < -32766 ) x = -32766;
			if( x > 32766 ) x = 32766;
			
			ultrabuffer_input[i + y] = (short)x;
		}
	}

	virtual int32_t RecordedDataIsAvailable(
						const void* audioSamples,
						const size_t nSamples,
						const size_t nBytesPerSample,
						const size_t nChannels,
						const uint32_t samplesPerSec,
						const uint32_t totalDelayMS,
						const int32_t clockDrift,
						const uint32_t currentMicLevel,
						const bool keyPressed,
						uint32_t& newMicLevel )
	{
		short *s = (short*)audioSamples;
		
		float inpcm[960];
		float outpcm[960];
		float opcm[960];
		short endpcm[960];

		int rc;
		
		float f;
		float o;
		float max;
		max = 0.2f;
		float mul;
		float inc;
		
		size_t i;
		for( i = 0; i < nSamples * nChannels; i += nChannels )
		{
			f = (float)s[i] / 32768.0;
			o = f;
			hi += (f - hi) * 0.02;
			f -= hi;
			ohi += (o - ohi) * 0.4;
			o -= ohi;
			f = (f * 0.7) + (o * 1.5);
			inpcm[i / nChannels] = f;
			if( f < 0.0 ) f = -f;
			if( f > max ) max = f;
		}
		
		mul = 1.0 / max;
		inc = (mul - nmul) / 480.0;
		if( inc > 0.0 ) inc /= 4.0;
		
		rsmpl( inpcm, outpcm, (int)nSamples, 480 );

		const float* const buffers[] = {outpcm, 0};
		float* const output[] = {opcm, 0};
		
		apm->set_stream_delay_ms( totalDelayMS );
		//apm->echo_cancellation()->set_stream_drift_samples( clockDrift );
		apm->ProcessStream( buffers, 480, 48000, webrtc::AudioProcessing::kMono, 48000, webrtc::AudioProcessing::kMono, output );

		int minimum[3];
		int maximum[3];
		int wert;
		int index;
		for( index = 0; index < 3; index++ ) {
			minimum[index] = 200;
			maximum[index] = 0;
		}
		index = 0;
		
		for( i = 0; i < 480; i++ )
		{
			nmul += inc;
			f = opcm[i] * nmul;
			
			if( f < -1.0 ) f = -1.0;
			if( f > 1.0 ) f = 1.0;
			
			endpcm[i] = (short)(f * 32766.0);
			
			if( i == 160 ) index = 1;
			if( i == 320 ) index = 2;
			
			wert = (int)(100.0 + (f * 100.0));
			if( wert < minimum[index] ) minimum[index] = wert;
			if( wert > maximum[index] ) maximum[index] = wert;
		}
		
		outpacket.minimax[0] = (unsigned char)minimum[0];
		outpacket.minimax[1] = (unsigned char)maximum[0];
		outpacket.minimax[2] = (unsigned char)minimum[1];
		outpacket.minimax[3] = (unsigned char)maximum[1];
		outpacket.minimax[4] = (unsigned char)minimum[2];
		outpacket.minimax[5] = (unsigned char)maximum[2];
		rc = opus_encode( enc, endpcm, 480, outpacket.opus, 250 );
		outpacket.length = (unsigned char)rc;
		send( sock, (char*)&outpacket, rc+7, 0 );
		
		return 0;
	}


	virtual int32_t NeedMorePlayData(const size_t nSamples,
					   const size_t nBytesPerSample,
					   const size_t nChannels,
					   const uint32_t samplesPerSec,
					   void* audioSamples,
					   size_t& nSamplesOut,
					   int64_t* elapsed_time_ms,
					   int64_t* ntp_time_ms)
	{
		float f;
		float inpcm[960];
		float outpcm[960];
		short *endpcm = (short*)audioSamples;

		size_t i;
		
		for( i = 0; i < 480; i++ )
		{
			f = (float)ultrabuffer_input[i] / 32768.0;
			inpcm[i] = f;
		}

		for( i = 0; i < 1999000; i++ )
			ultrabuffer_input[i] = ultrabuffer_input[i+480];
		
		for( i = 0; i < 30; i++ )
		{
			if( client_online[i] ) {
				client_position[i] -= 10;
				if( client_position[i] <= 0 ) {
					client_online[i] = false;
				}
			}
		}
		
		const float* const buffers[] = {inpcm, 0};
		apm->AnalyzeReverseStream( buffers, 480, 48000, webrtc::AudioProcessing::kMono );

		rsmpl( inpcm, outpcm, 480, (int)nSamples );
		
		if( nChannels == 2 )
		{
			for( i = 0; i < nSamples; i++ )
			{
				endpcm[i+i] = (short)(outpcm[i] * 32766.0);
				endpcm[i+i+1] = endpcm[i+i];
			}
		}
		if( nChannels == 1 )
		{
			for( i = 0; i < nSamples; i++ )
				endpcm[i] = (short)(outpcm[i] * 32766.0);
		}
		
		nSamplesOut = nSamples;
		return 0;
	}

	virtual void PushCaptureData(int voe_channel,
					   const void* audio_data,
					   int bits_per_sample,
					   int sample_rate,
					   size_t number_of_channels,
					   size_t number_of_frames)
	{
		puts( "PushCaptureData" );
		Sleep( 1000 );
	}

	virtual void PullRenderData(int bits_per_sample,
					  int sample_rate,
					  size_t number_of_channels,
					  size_t number_of_frames,
					  void* audio_data,
					  int64_t* elapsed_time_ms,
					  int64_t* ntp_time_ms)
	{
		puts( "PullRenderData" );
		Sleep( 1000 );
	}

};

int main( int argc, char *argv[] )
{
	if( argc != 3 )
		return 0;
	
	FreeConsole();
	
	char target[1024];
	memset( target, 0, 1024 );
	strcpy( target, "audio " );
	strcat( target, argv[1] );
	strcat( target, " " );
	strcat( target, argv[2] );
	strcat( target, "\n" );
	int tlen;
	tlen = strlen( target );
	
	WSAStartup( MAKEWORD(2,2), &wsa );
	
	int i;
	int rc;
	
	for( i = 0; i < 30; i++ ) {
		client_online[i] = false;
		client_dec[i] = opus_decoder_create( 48000, 1, &rc );
		client_fec[i] = 0;
	}
	
	sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	memset( &addr, 0, sizeof(SOCKADDR_IN) );
	addr.sin_family = AF_INET;
	addr.sin_port = htons( 9100 );
	addr.sin_addr.s_addr = inet_addr( "188.68.38.124" );
	
	if( connect( sock, (SOCKADDR*)&addr, sizeof(addr) ) < 0 )
		return 0;

	send( sock, target, tlen, 0 );
	
	apm = webrtc::AudioProcessing::Create();
	apm->noise_suppression()->set_level( webrtc::NoiseSuppression::kHigh );
	apm->noise_suppression()->Enable( true );
	apm->echo_cancellation()->set_suppression_level( webrtc::EchoCancellation::kHighSuppression );
	apm->echo_cancellation()->Enable( true );
	apm->Initialize( 48000, 48000, 48000, webrtc::AudioProcessing::kMono, webrtc::AudioProcessing::kMono, webrtc::AudioProcessing::kMono );

	rtc::scoped_refptr<webrtc::AudioDeviceModule> audio = webrtc::AudioDeviceModule::Create( 0, webrtc::AudioDeviceModule::kPlatformDefaultAudio );
	
	audio->Init();

	audio->SetPlayoutDevice( webrtc::AudioDeviceModule::kDefaultCommunicationDevice );
	audio->InitPlayout();
	
	audio->SetRecordingDevice( webrtc::AudioDeviceModule::kDefaultCommunicationDevice );
	audio->InitRecording();

	AudioTransportImpl *callback = new AudioTransportImpl();
	audio->RegisterAudioCallback( callback );
	
	audio->StartRecording();
	audio->StartPlayout();

	struct timeval tv;
	
	fd_set master, readfd;
	FD_ZERO( &master );
	FD_ZERO( &readfd );
	FD_SET( sock, &master );

	short pcm[480];
	
	IPACKET inpacket;
	unsigned char inraw[252];
	int index;
	index = -1;
	
	int speedupphase;
	speedupphase = 0;
	
	int start_time, end_time, diff_time;
	start_time = GetTickCount();
	
	while( true ) {
		readfd = master;
		tv.tv_sec = 0;
		tv.tv_usec = 16000;
		select( 0, &readfd, 0, 0, &tv );
		
		end_time = GetTickCount();
		diff_time = end_time - start_time;
		
		if( diff_time > 20 ) {
			start_time = end_time;
			for( i = 0; i < 30; i++ ) {
				if( client_online[i] ) {
					if( client_position[i] < 100 ) {
						client_fec[i] = 1;
						rc = opus_decode( client_dec[i], 0, 0, pcm, 480, 1 );
						callback->put( pcm, client_position[i] );
					}
				}
			}
		}
		
		if( !FD_ISSET(sock, &readfd) )
			continue;
		
		index++;
		rc = recv( sock, (char*)&inraw[index], 1, 0 );
		if( rc != 1 ) {
			puts("recv");
			break;
		}
		
		if( index == 1 ) {
			inpacket.ID = inraw[0];
			inpacket.length = inraw[1];
			
			rc = (unsigned int)inpacket.ID;
			if( rc >= 30 ) {
				puts("id>=30");
				break;
			}
			
			i = (int)inpacket.ID;
			if( !client_online[i] ) {
				client_position[i] = 200;
				client_online[i] = true;
			}
		}
		
		if( index >= 2 ) {
			inpacket.opus[index-2] = inraw[index];
			rc = (unsigned int)inpacket.length;
			rc += 1;
			if( index == rc ) {
				index = -1;
				i = (int)inpacket.ID;
				rc = opus_decode( client_dec[i], inpacket.opus, rc-1, pcm, 480, client_fec[i] );
				if( client_position[i] < 2500 ) {
					callback->put( pcm, client_position[i] );
					client_position[i] += 10;
					client_fec[i] = 0;
				} else
					client_fec[i] = 1;
			}
		}
	}

	audio->StopPlayout();
	audio->StopRecording();
	
	delete callback;
	closesocket( sock );

	return 0;
}
