/*++
    FilterPolicy.h

    Driver-wide policy state and the in-memory redaction engine.
    Everything here must be safe to call from a completion routine, i.e.
    from PASSIVE_LEVEL up to DISPATCH_LEVEL.
--*/

#pragma once

#include <ntddk.h>
#include "../include/TempModShared.h"

typedef struct _TEMPMOD_STATE {
    KSPIN_LOCK     Lock;          /* protects Policy + counters            */
    TEMPMOD_POLICY Policy;        /* current armed policy, always valid    */
    LONG           RequestsObserved;
    LONG           BuffersScanned;
    LONG           ReplacementsTotal;
} TEMPMOD_STATE, *PTEMPMOD_STATE;

/* Zeroes the state, initialises the lock, disarms the policy. */
VOID
TempModInitializeState(
    _Out_ PTEMPMOD_STATE State
    );

/* Returns STATUS_SUCCESS / STATUS_REVISION_MISMATCH / STATUS_INVALID_PARAMETER. */
NTSTATUS
TempModValidatePolicy(
    _In_ const TEMPMOD_POLICY* Policy
    );

/* Validates, normalises and stores a copy of the policy. */
NTSTATUS
TempModSetPolicy(
    _Inout_ PTEMPMOD_STATE        State,
    _In_    const TEMPMOD_POLICY* Policy
    );

/* IRQL-safe snapshot copy for the data path. */
VOID
TempModGetPolicy(
    _Inout_ PTEMPMOD_STATE  State,
    _Out_   TEMPMOD_POLICY* Policy
    );

/* Disarms the policy, keeping the counters. */
VOID
TempModClearPolicy(
    _Inout_ PTEMPMOD_STATE State
    );

/* IRQL-safe counter snapshot. */
VOID
TempModGetStats(
    _Inout_ PTEMPMOD_STATE State,
    _Out_   TEMPMOD_STATS* Stats
    );

/* Zeroes every counter. */
VOID
TempModResetStats(
    _Inout_ PTEMPMOD_STATE State
    );

/*
    Overwrites every occurrence of Policy->Pattern inside Buffer with
    Policy->Replacement. Returns the number of replacements performed.

    The caller must guarantee that Buffer is writable, non-paged and that
    Length describes the *valid* part of the buffer.
*/
ULONG
TempModRedactBuffer(
    _Inout_updates_bytes_(Length) PUCHAR                Buffer,
    _In_                          size_t                Length,
    _In_                          const TEMPMOD_POLICY* Policy
    );
