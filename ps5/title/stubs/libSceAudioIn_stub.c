/* Link metadata only for the resident libSceAudioIn system module.
 * The product game parent captures only while the touchpad is held. */
__asm__(".global sceAudioInInit\n.type sceAudioInInit @function\nsceAudioInInit:\n");
__asm__(".global sceAudioInOpen\n.type sceAudioInOpen @function\nsceAudioInOpen:\n");
__asm__(".global sceAudioInInput\n.type sceAudioInInput @function\nsceAudioInInput:\n");
__asm__(".global sceAudioInClose\n.type sceAudioInClose @function\nsceAudioInClose:\n");
