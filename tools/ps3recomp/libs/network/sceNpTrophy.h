/*
 * ps3recomp - sceNpTrophy HLE
 *
 * Trophy/achievement system: create contexts, unlock trophies, query progress.
 * The trophy set (names, grades, hidden flags, icons) comes from the game's own
 * disc: PS3_GAME/TROPDIR/<NPCOMMID>_00/TROPHY.TRP. Unlocks are kept on the PS3
 * hard drive, dev_hdd0/home/00000001/trophy/<NPCOMMID>_00/, as on a console.
 */

#ifndef PS3RECOMP_SCE_NP_TROPHY_H
#define PS3RECOMP_SCE_NP_TROPHY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "sceNp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (the system's: a title compares against these, e.g. A Crack in
 * Time takes ALREADY_UNLOCKED as "done" and anything else as "try again").
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED     0x80022901
#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED         0x80022902
#define SCE_NP_TROPHY_ERROR_NOT_SUPPORTED           0x80022903
#define SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED  0x80022904
#define SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY           0x80022905
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT        0x80022906
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX             0x80022907
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT            0x80022909
#define SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT         0x8002290A
#define SCE_NP_TROPHY_ERROR_INVALID_FORMAT          0x8002290B
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT         0x8002290E
#define SCE_NP_TROPHY_ERROR_ABORT                   0x80022910
#define SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE          0x80022911
#define SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM  0x80022914
#define SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED        0x80022915
#define SCE_NP_TROPHY_ERROR_INVALID_HANDLE          0x80022917
#define SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID      0x80022918
#define SCE_NP_TROPHY_ERROR_UNKNOWN_NP_COMM_ID      0x80022919

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_INVALID_CONTEXT       ((SceNpTrophyContext)0)
#define SCE_NP_TROPHY_INVALID_HANDLE        ((SceNpTrophyHandle)0)
#define SCE_NP_TROPHY_INVALID_TROPHY_ID     ((SceNpTrophyId)-1)

#define SCE_NP_TROPHY_MAX_NUM_TROPHIES      128
#define SCE_NP_TROPHY_NAME_MAX_SIZE         128
#define SCE_NP_TROPHY_DESC_MAX_SIZE         1024
#define SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE   128
#define SCE_NP_TROPHY_GAME_DESC_MAX_SIZE    1024

#define SCE_NP_TROPHY_MAX_CONTEXTS          4
#define SCE_NP_TROPHY_MAX_HANDLES           4

/* Trophy grades */
#define SCE_NP_TROPHY_GRADE_UNKNOWN         0
#define SCE_NP_TROPHY_GRADE_PLATINUM        1
#define SCE_NP_TROPHY_GRADE_GOLD            2
#define SCE_NP_TROPHY_GRADE_SILVER          3
#define SCE_NP_TROPHY_GRADE_BRONZE          4

/* ---------------------------------------------------------------------------
 * Types. These are the guest's (big-endian) layouts: the handlers write them
 * field by field with vm_write*, never as host structs.
 *   SceNpTrophyDetails      0 id, 4 grade, 8 name[128], 136 description[1024],
 *                           1160 hidden, 1161 reserved[3]           (1164)
 *   SceNpTrophyData         0 timestamp (CellRtcTick), 8 id, 12 unlocked,
 *                           13 reserved[3]                          (16)
 *   SceNpTrophyGameDetails  0 numTrophies, 4 platinum, 8 gold, 12 silver,
 *                           16 bronze, 20 title[128], 148 description[1024],
 *                           1172 reserved[4]                        (1176)
 *   SceNpTrophyGameData     unlocked: 0 all, 4 platinum, 8 gold, 12 silver,
 *                           16 bronze                               (20)
 *   SceNpTrophyFlagArray    u32 flag_bits[4]: trophy i = bit (i % 32) of
 *                           word i / 32                              (16)
 * -----------------------------------------------------------------------*/
typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef s32 SceNpTrophyId;

typedef struct SceNpTrophyDetails SceNpTrophyDetails;
typedef struct SceNpTrophyData SceNpTrophyData;
typedef struct SceNpTrophyGameDetails SceNpTrophyGameDetails;
typedef struct SceNpTrophyGameData SceNpTrophyGameData;
typedef struct SceNpTrophyFlagArray SceNpTrophyFlagArray;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options);
s32 sceNpTrophyTerm(void);

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options);
s32 sceNpTrophyDestroyContext(SceNpTrophyContext context);

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle);
s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle);

/* Real ABI: (context, handle, statusCb, arg, options).  statusCb is a guest
 * OPD for SceNpTrophyStatusCallback(context, status, completed, total, arg);
 * the game blocks on registration-complete which is signalled via this cb. */
s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options);

/* SceNpTrophyStatus values fired to the status callback */
#define SCE_NP_TROPHY_STATUS_UNKNOWN            0
#define SCE_NP_TROPHY_STATUS_NOT_INSTALLED      1
#define SCE_NP_TROPHY_STATUS_DATA_CORRUPT       2
#define SCE_NP_TROPHY_STATUS_INSTALLED          3
#define SCE_NP_TROPHY_STATUS_REQUIRES_UPDATE    4
#define SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE 8

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options);

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data);

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data);

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId);

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count);

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage);

/* Kept for the NID table; the storage is the PS3 hard drive now (a guest
 * call to this is ignored). */
void sceNpTrophySetStoragePath(const char* path);

/* ---------------------------------------------------------------------------
 * Host side (not guest calls)
 * -----------------------------------------------------------------------*/
/* Unlock a trophy of the registered set from host code: a title's own rule
 * (e.g. a trophy the game asks for only at a higher tier). Same as the
 * game's unlock: saved, the platinum checked, the notice shown. Returns 0,
 * 1 if it was unlocked already, -1 if there is no such trophy or no set. */
int sceNpTrophyHostUnlock(int trophyId, const char* why);
/* Called after every unlock (the game's or the host's), outside any lock,
 * on the unlocking thread. */
void sceNpTrophySetUnlockHook(void (*hook)(int trophyId));

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TROPHY_H */
