/*++
    FilterPolicy.c

    The actual "temporary modification" engine.  Nothing in this file writes
    to any medium: the only thing that ever changes is a buffer that already
    lives in RAM and belongs to a single in-flight I/O request.

    IRQL contract:
        * TempModInitializeState / TempModResetStats : PASSIVE_LEVEL
        * TempModGetPolicy / TempModRedactBuffer / TempModApplyPolicy : <= DISPATCH_LEVEL
        * everything else                            : PASSIVE_LEVEL
--*/
#include "FilterPolicy.h"

/* ------------------------------------------------------------------------- */

static VOID
TempModAcquire(
    _Inout_ PTEMPMOD_STATE State,
    _Out_ PKIRQL OldIrql,
    _Out_ PBOOLEAN AtDispatch
    )
{
    *AtDispatch = (KeGetCurrentIrql() >= DISPATCH_LEVEL);

    if (*AtDispatch) {
        KeAcquireSpinLockAtDpcLevel(&State->Lock);
    } else {
        KeAcquireSpinLock(&State->Lock, OldIrql);
    }
}

static VOID
TempModRelease(
    _Inout_ PTEMPMOD_STATE State,
    _In_ KIRQL OldIrql,
    _In_ BOOLEAN AtDispatch
    )
{
    if (AtDispatch) {
        KeReleaseSpinLockFromDpcLevel(&State->Lock);
    } else {
        KeReleaseSpinLock(&State->Lock, OldIrql);
    }
}

/* ------------------------------------------------------------------------- */

VOID
TempModInitializeState(
    _Out_ PTEMPMOD_STATE State
    )
{
    RtlZeroMemory(State, sizeof(*State));
    KeInitializeSpinLock(&State->Lock);

    State->Policy.Version = TEMPMOD_POLICY_VERSION;
    State->Policy.Enabled = 0;
    State->Policy.RuleCount = 0;
    State->Policy.MaxReplacementsPerRequest = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;
}

NTSTATUS
TempModValidatePolicy(
    _In_ const TEMPMOD_POLICY* Policy
    )
{
    ULONG i;

    if (Policy == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Policy->Version != TEMPMOD_POLICY_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    if (Policy->Flags != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Policy->RuleCount > TEMPMOD_MAX_RULES) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Policy->MaxReplacementsPerRequest > TEMPMOD_MAX_REPLACEMENTS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Policy->Enabled != 0 && Policy->RuleCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < Policy->RuleCount; i++) {
        const TEMPMOD_RULE* rule = &Policy->Rules[i];

        if (rule->PatternLength == 0 ||
            rule->PatternLength > TEMPMOD_PATTERN_MAX) {
            return STATUS_INVALID_PARAMETER;
        }

        if (rule->ReplacementLength != 0 &&
            rule->ReplacementLength > TEMPMOD_PATTERN_MAX) {
            return STATUS_INVALID_PARAMETER;
        }

        if (rule->ReplacementLength != 0 &&
            rule->ReplacementLength > rule->PatternLength) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
TempModSetPolicy(
    _Inout_ PTEMPMOD_STATE State,
    _In_ const TEMPMOD_POLICY* Policy
    )
{
    TEMPMOD_POLICY normalised;
    KIRQL oldIrql = PASSIVE_LEVEL;
    BOOLEAN atDispatch;
    NTSTATUS status;
    ULONG i;

    status = TempModValidatePolicy(Policy);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&normalised, sizeof(normalised));
    normalised = *Policy;

    if (normalised.RuleCount > TEMPMOD_MAX_RULES) {
        normalised.RuleCount = TEMPMOD_MAX_RULES;
    }

    for (i = 0; i < normalised.RuleCount; i++) {
        TEMPMOD_RULE* rule = &normalised.Rules[i];

        if (rule->PatternLength > TEMPMOD_PATTERN_MAX) {
            rule->PatternLength = TEMPMOD_PATTERN_MAX;
        }

        if (rule->PatternLength < TEMPMOD_PATTERN_MAX) {
            RtlZeroMemory(
                &rule->Pattern[rule->PatternLength],
                TEMPMOD_PATTERN_MAX - rule->PatternLength);
        }

        if (rule->ReplacementLength == 0) {
            rule->ReplacementLength = rule->PatternLength;
            if (rule->ReplacementLength > 0 &&
                rule->Replacement[0] == 0) {
                RtlFillMemory(rule->Replacement,
                              rule->ReplacementLength, (UCHAR)'*');
            } else if (rule->ReplacementLength > 0) {
                RtlFillMemory(rule->Replacement,
                              rule->ReplacementLength,
                              rule->Replacement[0]);
            }
        }
        if (rule->ReplacementLength > rule->PatternLength) {
            rule->ReplacementLength = rule->PatternLength;
        }
        if (rule->ReplacementLength < TEMPMOD_PATTERN_MAX) {
            RtlZeroMemory(
                &rule->Replacement[rule->ReplacementLength],
                TEMPMOD_PATTERN_MAX - rule->ReplacementLength);
        }
    }

    if (normalised.MaxReplacementsPerRequest == 0) {
        normalised.MaxReplacementsPerRequest = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;
    }
    if (normalised.MaxReplacementsPerRequest > TEMPMOD_MAX_REPLACEMENTS) {
        normalised.MaxReplacementsPerRequest = TEMPMOD_MAX_REPLACEMENTS;
    }

    normalised.Flags = 0;
    normalised.Enabled = (normalised.Enabled != 0 && normalised.RuleCount > 0) ? 1 : 0;

    TempModAcquire(State, &oldIrql, &atDispatch);
    State->Policy = normalised;
    TempModRelease(State, oldIrql, atDispatch);

    return STATUS_SUCCESS;
}

VOID
TempModGetPolicy(
    _Inout_ PTEMPMOD_STATE  State,
    _Out_   TEMPMOD_POLICY* Policy
    )
{
    KIRQL oldIrql = PASSIVE_LEVEL;
    BOOLEAN atDispatch;

    TempModAcquire(State, &oldIrql, &atDispatch);
    *Policy = State->Policy;
    TempModRelease(State, oldIrql, atDispatch);
}

VOID
TempModClearPolicy(
    _Inout_ PTEMPMOD_STATE State
    )
{
    KIRQL oldIrql = PASSIVE_LEVEL;
    BOOLEAN atDispatch;

    TempModAcquire(State, &oldIrql, &atDispatch);
    State->Policy.Enabled = 0;
    State->Policy.RuleCount = 0;
    RtlZeroMemory(State->Policy.Rules, sizeof(State->Policy.Rules));
    TempModRelease(State, oldIrql, atDispatch);
}

VOID
TempModGetStats(
    _Inout_ PTEMPMOD_STATE State,
    _Out_   TEMPMOD_STATS* Stats
    )
{
    KIRQL oldIrql = PASSIVE_LEVEL;
    BOOLEAN atDispatch;

    RtlZeroMemory(Stats, sizeof(*Stats));
    Stats->Version = TEMPMOD_STATS_VERSION;

    TempModAcquire(State, &oldIrql, &atDispatch);
    Stats->RequestsObserved   = (ULONG)State->RequestsObserved;
    Stats->BuffersScanned     = (ULONG)State->BuffersScanned;
    Stats->ReplacementsTotal  = (ULONG)State->ReplacementsTotal;
    Stats->PolicyArmed        = State->Policy.Enabled;
    TempModRelease(State, oldIrql, atDispatch);
}

VOID
TempModResetStats(
    _Inout_ PTEMPMOD_STATE State
    )
{
    InterlockedExchange(&State->RequestsObserved, 0);
    InterlockedExchange(&State->BuffersScanned, 0);
    InterlockedExchange(&State->ReplacementsTotal, 0);
}

/* ------------------------------------------------------------------------- */

ULONG
TempModRedactBuffer(
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ size_t Length,
    _In_ const TEMPMOD_RULE* Rule
    )
{
    size_t index;
    size_t patternLength;
    ULONG replacements;
    ULONG limit;

    if (Buffer == NULL || Rule == NULL) {
        return 0;
    }

    patternLength = Rule->PatternLength;

    if (patternLength == 0 ||
        patternLength > TEMPMOD_PATTERN_MAX ||
        patternLength > Length) {
        return 0;
    }

    limit = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;

    replacements = 0;

    for (index = 0;
         (index + patternLength) <= Length && replacements < limit;
         index++) {

        if (Buffer[index] != Rule->Pattern[0]) {
            continue;
        }

        if (RtlCompareMemory(Buffer + index, Rule->Pattern, patternLength)
                != patternLength) {
            continue;
        }

        {
            size_t replLen = Rule->ReplacementLength;
            if (replLen == 0) {
                replLen = patternLength;
            }
            if (replLen > patternLength) {
                replLen = patternLength;
            }
            for (size_t k = 0; k < patternLength; k++) {
                if (k < replLen) {
                    Buffer[index + k] = Rule->Replacement[k];
                } else {
                    Buffer[index + k] = (UCHAR)' ';
                }
            }
        }
        replacements++;
        index += (patternLength - 1);
    }

    return replacements;
}

ULONG
TempModApplyPolicy(
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ size_t Length,
    _In_ ULONG IoControl,
    _In_ const TEMPMOD_POLICY* Policy
    )
{
    ULONG total = 0;
    ULONG i;
    ULONG limit;

    if (Buffer == NULL || Policy == NULL || !Policy->Enabled) {
        return 0;
    }

    limit = Policy->MaxReplacementsPerRequest;
    if (limit == 0) {
        limit = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;
    }

    for (i = 0; i < Policy->RuleCount && i < TEMPMOD_MAX_RULES; i++) {
        const TEMPMOD_RULE* rule = &Policy->Rules[i];

        if (rule->PatternLength == 0) {
            continue;
        }
        if (rule->TargetIoControl != 0 &&
            rule->TargetIoControl != IoControl) {
            continue;
        }

        total += TempModRedactBuffer(Buffer, Length, rule);
        if (total >= limit) {
            break;
        }
    }

    return total;
}