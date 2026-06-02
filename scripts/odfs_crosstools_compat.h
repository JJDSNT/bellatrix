/*
 * scripts/odfs_crosstools_compat.h
 * Passed via -include when building ODFileSystem with amigadev/crosstools.
 *
 * The crosstools NDK's proto/utility.h declares UtilityBase as
 * 'extern struct UtilityBase *UtilityBase' which conflicts with
 * handler_main.c's own 'struct Library *UtilityBase' definition.
 *
 * PROTO_UTILITY_H suppresses the entire utility proto header — safe because
 * MatchPatternNoCase is provided by dos.library (inline/dos.h) in this NDK,
 * not by utility.library. handler_main.c's own definition of UtilityBase
 * as 'struct Library *' is used.
 */
#define PROTO_UTILITY_H  1

/* TD_READ64 is absent from older NDK versions shipped with crosstools */
#ifndef TD_READ64
#define TD_READ64  0x18
#endif
