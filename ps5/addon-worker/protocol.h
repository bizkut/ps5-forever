/* Forever Launcher native add-on worker wire protocol. */
#ifndef FOREVER_ADDON_WORKER_PROTOCOL_H
#define FOREVER_ADDON_WORKER_PROTOCOL_H

#define AW_PORT 9142u
#define AW_MAGIC 0x31574146u /* "FAW1" little-endian */
#define AW_VERSION 2u
#define AW_TOKEN_BYTES 64u
#define AW_OPERATION_ID_BYTES 32u
#define AW_CHALLENGE_BYTES 32u
#define AW_CHUNK_BYTES (1u << 20)
#define AW_HELLO_BYTES 8u
#define AW_CHALLENGE_REPLY_BYTES 40u
#define AW_AUTH_BYTES 32u
#define AW_AUTH_REPLY_BYTES 44u
#define AW_STARTUP_BYTES 72u
#define AW_REQUEST_BYTES 88u
#define AW_REPLY_BYTES 100u

#define AW_COMMAND_PROBE 1u
#define AW_COMMAND_STORE 2u
#define AW_COMMAND_EXTRACT 3u
#define AW_COMMAND_CLEAN 4u
#define AW_COMMAND_CLEAN_ALL 5u
#define AW_COMMAND_QUIT 6u
#define AW_COMMAND_ACTIVATE 7u

#define AW_PHASE_READY 1u
#define AW_PHASE_PROGRESS 2u
#define AW_PHASE_COMPLETE 3u

#define AW_NATIVE_ROOT "/data/forever/prefix/drive_c/Forever/AddonStaging"
#define AW_WINDOWS_ROOT "C:\\Forever\\AddonStaging"
#define AW_RELATIVE_ELF "addon-worker.elf"

#endif
