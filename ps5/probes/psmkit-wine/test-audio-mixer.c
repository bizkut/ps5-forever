/* Host tests for Wine's allocation-free PS5 title audio mixer. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mixer.h"

static float mixed[2 * PS5_AUDIO_GRAIN];

static int near( float left, float right )
{
    return fabsf( left - right ) < 0.0001f;
}

static struct ps5_audio_format pcm( uint32_t rate, uint16_t channels, uint16_t bits )
{
    struct ps5_audio_format format =
    {
        rate, channels, bits, channels * bits / 8u, PS5_AUDIO_PCM, 0
    };
    return format;
}

static uint32_t mix( struct ps5_audio_mix_stream *stream, uint32_t frames )
{
    memset( mixed, 0, sizeof(mixed) );
    return ps5_audio_mix_add( stream, mixed, frames );
}

static void test_formats(void)
{
    struct ps5_audio_format format = pcm( 48000, 2, 16 );
    struct ps5_audio_mix_stream stream;
    uint8_t ring[16];

    assert( ps5_audio_mix_format_check( NULL ) == PS5_AUDIO_MIX_PRECONDITION );
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_OK );
    format.bits = 12;
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_UNSUPPORTED );
    format = pcm( 999, 2, 16 );
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_UNSUPPORTED );
    format = pcm( 384001, 2, 16 );
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_UNSUPPORTED );
    format = pcm( 48000, 9, 16 );
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_UNSUPPORTED );
    format = pcm( 48000, 2, 32 );
    format.encoding = PS5_AUDIO_FLOAT;
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_OK );
    format.block_align = 4;
    assert( ps5_audio_mix_format_check( &format ) == PS5_AUDIO_MIX_UNSUPPORTED );

    format = pcm( 48000, 2, 16 );
    assert( ps5_audio_mix_stream_init( NULL, &format, ring, 4 ) == PS5_AUDIO_MIX_PRECONDITION );
    assert( ps5_audio_mix_stream_init( &stream, &format, NULL, 4 ) == PS5_AUDIO_MIX_PRECONDITION );
    assert( ps5_audio_mix_stream_init( &stream, &format, ring, 0 ) == PS5_AUDIO_MIX_PRECONDITION );
    assert( ps5_audio_mix_stream_init( &stream, &format, ring, 4 ) == PS5_AUDIO_MIX_OK );
    assert( !stream.held && !stream.offset && !stream.consumed );
    assert( !mix( &stream, PS5_AUDIO_GRAIN ) );
}

static void test_samples_and_grain(void)
{
    int16_t ring[PS5_AUDIO_GRAIN * 2];
    struct ps5_audio_format format = pcm( 48000, 2, 16 );
    struct ps5_audio_mix_stream stream;
    uint32_t frame;

    for (frame = 0; frame < PS5_AUDIO_GRAIN; frame++)
    {
        ring[2 * frame] = frame * 64;
        ring[2 * frame + 1] = -(int16_t)(frame * 64);
    }
    assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)ring,
                                       PS5_AUDIO_GRAIN ) == PS5_AUDIO_MIX_OK );
    stream.held = PS5_AUDIO_GRAIN;
    assert( mix( &stream, PS5_AUDIO_GRAIN ) == PS5_AUDIO_GRAIN );
    assert( !mixed[0] && !mixed[1] );
    assert( near( mixed[2], ring[0] / 32768.0f ) );
    assert( near( mixed[2 * (PS5_AUDIO_GRAIN - 1)], ring[2 * (PS5_AUDIO_GRAIN - 2)] / 32768.0f ) );
    assert( !stream.held && stream.consumed == PS5_AUDIO_GRAIN );
    assert( !mix( &stream, PS5_AUDIO_GRAIN ) );

    {
        uint8_t samples[] = {128, 128, 255, 0, 0, 192};
        format = pcm( 48000, 2, 8 );
        assert( ps5_audio_mix_stream_init( &stream, &format, samples, 3 ) == PS5_AUDIO_MIX_OK );
        stream.held = 3;
        assert( mix( &stream, 4 ) == 3 );
        assert( near( mixed[4], 127.0f / 128.0f ) && near( mixed[5], -1.0f ) );
    }
    {
        uint8_t samples[] = {0,0,0x40, 0,0,0xc0, 0xff,0xff,0x7f, 0,0,0x80};
        format = pcm( 48000, 2, 24 );
        assert( ps5_audio_mix_stream_init( &stream, &format, samples, 2 ) == PS5_AUDIO_MIX_OK );
        stream.held = 2;
        assert( mix( &stream, 3 ) == 2 );
        assert( near( mixed[2], 0.5f ) && near( mixed[3], -0.5f ) );
    }
    {
        float samples[] = {0.25f, -0.75f, NAN, INFINITY, 2.0f, -2.0f};
        format = pcm( 48000, 2, 32 );
        format.encoding = PS5_AUDIO_FLOAT;
        assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)samples, 3 ) == PS5_AUDIO_MIX_OK );
        stream.held = 3;
        assert( mix( &stream, 4 ) == 3 );
        assert( near( mixed[2], 0.25f ) && near( mixed[3], -0.75f ) );
        assert( !mixed[4] && near( mixed[5], 1.0f ) );
    }
}

static void test_fold_gain_and_store(void)
{
    int16_t six[12] = {16384,8192,16384,32767,8192,16384, 0,0,0,0,0,0};
    struct ps5_audio_format format = pcm( 48000, 6, 16 );
    struct ps5_audio_mix_stream stream;
    const float side = 0.70710678f;
    float gain[6] = {0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    int16_t output[4];
    const float clipped[4] = {1.25f, -1.25f, 0.5f, -0.5f};

    assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)six, 2 ) == PS5_AUDIO_MIX_OK );
    stream.held = 2;
    assert( mix( &stream, 2 ) == 2 );
    assert( near( mixed[2], 0.5f + 0.5f * side + 0.25f * side ) );
    assert( near( mixed[3], 0.25f + 0.5f * side + 0.5f * side ) );

    assert( ps5_audio_mix_set_gain( &stream, gain, 6 ) == PS5_AUDIO_MIX_OK );
    stream.offset = 0;
    stream.held = 2;
    ps5_audio_mix_stream_reset( &stream );
    stream.held = 2;
    assert( mix( &stream, 2 ) == 2 );
    assert( near( mixed[2], 0.25f + 0.5f * side + 0.25f * side ) );

    ps5_audio_mix_store( clipped, output, 2 );
    assert( output[0] == 32767 && output[1] == -32767 );
    assert( output[2] == 16384 && output[3] == -16384 );
}

static void test_resampling_wrap_and_reset(void)
{
    int16_t up[] = {16384, -16384, 0};
    int16_t down[] = {0,8192,16384,8192,0,-8192,-16384,-8192};
    int16_t ring[] = {1000,-1000, 2000,-2000, 3000,-3000};
    struct ps5_audio_format format;
    struct ps5_audio_mix_stream stream;
    static const float expected[] = {0, 0.25f, 0.5f, 0, -0.5f, -0.25f};
    unsigned int i;

    format = pcm( 24000, 1, 16 );
    assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)up, 3 ) == PS5_AUDIO_MIX_OK );
    stream.held = 3;
    assert( mix( &stream, 6 ) == 6 );
    for (i = 0; i < 6; i++)
        assert( near( mixed[2 * i], expected[i] ) && near( mixed[2 * i + 1], expected[i] ) );

    format = pcm( 96000, 1, 16 );
    assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)down, 8 ) == PS5_AUDIO_MIX_OK );
    stream.held = 8;
    assert( mix( &stream, 4 ) == 4 );
    assert( stream.consumed == 7 && stream.held == 1 );

    format = pcm( 48000, 2, 16 );
    assert( ps5_audio_mix_stream_init( &stream, &format, (const uint8_t *)ring, 3 ) == PS5_AUDIO_MIX_OK );
    stream.offset = 2;
    stream.held = 3;
    assert( mix( &stream, 2 ) == 2 );
    assert( near( mixed[2], 3000.0f / 32768.0f ) );
    assert( stream.offset == 1 && stream.held == 1 );
    assert( mix( &stream, 4 ) == 1 );
    assert( stream.offset == 2 && !stream.held );
    stream.held = 1;
    assert( mix( &stream, 1 ) == 1 && near( mixed[0], 2000.0f / 32768.0f ) );
    ps5_audio_mix_stream_reset( &stream );
    assert( !stream.offset && !stream.held && !stream.consumed );
}

int main(void)
{
    test_formats();
    test_samples_and_grain();
    test_fold_gain_and_store();
    test_resampling_wrap_and_reset();
    puts( "PS5_AUDIO_MIXER_PASS rate=48000 grain=256 format=s16-stereo" );
    return 0;
}
