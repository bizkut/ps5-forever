/* Tiny WinMM render probe for the PS5 title audio gate. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>

#define RATE 48000
#define FRAMES RATE

static int16_t samples[FRAMES * 2];

int main(void)
{
    WAVEFORMATEX format =
    {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = 2,
        .nSamplesPerSec = RATE,
        .nAvgBytesPerSec = RATE * 4,
        .nBlockAlign = 4,
        .wBitsPerSample = 16,
    };
    WAVEHDR header = {.lpData = (char *)samples, .dwBufferLength = sizeof(samples)};
    HWAVEOUT output = NULL;
    HANDLE done;
    MMRESULT result;
    unsigned int frame;
    int status = 1;

    /* A bounded 440-Hz square wave is enough to prove the complete WinMM ->
     * mmdevapi -> wineps5audio -> sceAudioOut render path by ear. */
    for (frame = 0; frame < FRAMES; frame++)
    {
        int16_t value = frame % (RATE / 440) < (RATE / 440) / 2 ? 8192 : -8192;
        samples[2 * frame] = samples[2 * frame + 1] = value;
    }
    if (!(done = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        printf( "PS5_AUDIO_TONE_FAIL stage=CreateEvent error=%lu\n", GetLastError() );
        return 1;
    }
    result = waveOutOpen( &output, WAVE_MAPPER, &format, (DWORD_PTR)done, 0, CALLBACK_EVENT );
    if (result != MMSYSERR_NOERROR)
    {
        printf( "PS5_AUDIO_TONE_FAIL stage=waveOutOpen mmresult=%u\n", result );
        goto done;
    }
    ResetEvent( done );
    if ((result = waveOutPrepareHeader( output, &header, sizeof(header) )) != MMSYSERR_NOERROR)
    {
        printf( "PS5_AUDIO_TONE_FAIL stage=waveOutPrepareHeader mmresult=%u\n", result );
        goto close;
    }
    if ((result = waveOutWrite( output, &header, sizeof(header) )) != MMSYSERR_NOERROR)
    {
        printf( "PS5_AUDIO_TONE_FAIL stage=waveOutWrite mmresult=%u\n", result );
        goto unprepare;
    }
    if (WaitForSingleObject( done, 5000 ) != WAIT_OBJECT_0 || !(header.dwFlags & WHDR_DONE))
    {
        printf( "PS5_AUDIO_TONE_FAIL stage=wait error=%lu flags=%#lx\n",
                GetLastError(), header.dwFlags );
        goto unprepare;
    }
    printf( "PS5_AUDIO_TONE_PASS rate=48000 frames=48000 channels=2 format=s16\n" );
    status = 0;

unprepare:
    waveOutReset( output );
    waveOutUnprepareHeader( output, &header, sizeof(header) );
close:
    waveOutClose( output );
done:
    CloseHandle( done );
    return status;
}
