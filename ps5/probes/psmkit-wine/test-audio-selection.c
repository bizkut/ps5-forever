/* Host test for title-only mmdevapi audio selection. */
#include <assert.h>
#include <stdio.h>

#include "ps5audio.h"

int main(void)
{
    assert( ps5_title_audio_mode_from_value( 0, 0 ) == -1 );
    assert( ps5_title_audio_mode_from_value( 2, '1' ) == -1 );
    assert( ps5_title_audio_mode_from_value( 1, '1' ) == 1 );
    assert( ps5_title_audio_mode_from_value( 1, '0' ) == 0 );
    assert( ps5_title_audio_mode_from_value( 1, 'x' ) == 0 );
    puts( "PS5_AUDIO_SELECTION_PASS game=ps5audio maintenance=default payload=registry" );
    return 0;
}
