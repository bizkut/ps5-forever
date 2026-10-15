/* Link-time stub for libSceIme, the console's inline keyboard, which the
 * payload SDK ships no stub for. Only the names matter: the title link turns
 * each imported name into its ID. Each is an export of 5.50's
 * libSceIme.sprx (library "G" of that module):
 *   sceImeOpen RPydv-Jr1bc, sceImeUpdate -4GCfYdNF1s, sceImeClose TmVP8LzcFcY,
 *   sceImeParamInit WmYDzdC4EHI, sceImeSetText ieCNrVrzKd4,
 *   sceImeSetCaret WLxUN2WMim8.
 * The module is not loaded with a title: load
 * /<sandbox word>/common/lib/libSceIme.sprx before the first call. */
__asm__(".global sceImeOpen\n.type sceImeOpen @function\nsceImeOpen:\n");
__asm__(".global sceImeUpdate\n.type sceImeUpdate @function\nsceImeUpdate:\n");
__asm__(".global sceImeClose\n.type sceImeClose @function\nsceImeClose:\n");
__asm__(".global sceImeParamInit\n.type sceImeParamInit @function\nsceImeParamInit:\n");
__asm__(".global sceImeSetText\n.type sceImeSetText @function\nsceImeSetText:\n");
__asm__(".global sceImeSetCaret\n.type sceImeSetCaret @function\nsceImeSetCaret:\n");
