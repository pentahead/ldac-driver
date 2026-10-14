/*
 * BtA2dpL2cap.sys - minimal KMDF Bluetooth profile driver that proxies L2CAP channels to user mode.
 *
 * STATUS: written against the documented WDK interfaces (bthddi.h, KMDF 1.15) but NOT YET COMPILED
 * OR TESTED in this repository (no WDK in the development environment). Build it with
 * scripts/build-drivers.ps1 or the CI workflow, then validate per docs/DRIVER.md before any install.
 *
 * Design (see docs/ARCHITECTURE.md):
 *  - Binds (only when explicitly selected by the switcher) to one remote sink's
 *    BTHENUM\{0000110B-...} devnode, replacing BthA2dp.sys for that device only.
 *  - Exposes GUID_DEVINTERFACE_BTA2DP_L2CAP with the IOCTL contract in include/bta2dp_ioctl.h.
 *  - Opens outgoing L2CAP channels to PSM 0x0019 on the bound remote device, and moves SDUs with
 *    BRB_L2CA_ACL_TRANSFER. No AVDTP, codec or audio logic lives in the kernel.
 *  - Version 1 limitations: no remote-initiated channels (ACCEPT returns STATUS_NOT_SUPPORTED),
 *    no flush-timeout configuration.
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>
#include <ntstrsafe.h>

#include "bta2dp_ioctl.h"

#define BTA_POOL_TAG 'AtB2'
#define BTA_PSM_AVDTP 0x0019
#define BTA_MIN_MTU 48

#define BtaLog(level, fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, level, "BtA2dpL2cap: " fmt "\n", __VA_ARGS__)

typedef struct _BTA_CHANNEL {
    BOOLEAN InUse;
    BOOLEAN Open;
    ULONG Id;
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT OutMtu;
    USHORT InMtu;
    LONG PendingSends;
    ULONGLONG BytesSent;
    ULONGLONG BytesReceived;
    struct _BTA_DEVICE_CONTEXT* Device;
} BTA_CHANNEL, *PBTA_CHANNEL;

typedef struct _BTA_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;
    BTH_PROFILE_DRIVER_INTERFACE Profile;
    BOOLEAN ProfileValid;
    BTH_ADDR RemoteAddress;
    WDFWAITLOCK ChannelLock;  /* PASSIVE_LEVEL: open/close/lookup */
    KSPIN_LOCK StatsLock;     /* DISPATCH_LEVEL: counters touched from completions */
    BTA_CHANNEL Channels[BTA2DP_MAX_CHANNELS];
    ULONG NextChannelId;
} BTA_DEVICE_CONTEXT, *PBTA_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTA_DEVICE_CONTEXT, BtaGetDeviceContext)

/* Per-transfer context carried by the child request used to submit an ACL BRB. */
typedef struct _BTA_TRANSFER {
    PBRB Brb;
    WDFREQUEST Original;
    PBTA_CHANNEL Channel;
    BOOLEAN Outbound;
} BTA_TRANSFER, *PBTA_TRANSFER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTA_TRANSFER, BtaGetTransfer)

/* Context attached to the user's (original) request so its cancel routine can reach the child. */
typedef struct _BTA_ORIGINAL {
    WDFREQUEST Child;  /* referenced while the original is cancelable */
} BTA_ORIGINAL, *PBTA_ORIGINAL;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTA_ORIGINAL, BtaGetOriginal)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD BtaEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE BtaEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE BtaEvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BtaEvtIoDeviceControl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE BtaTransferComplete;
EVT_WDF_REQUEST_CANCEL BtaEvtOriginalCancel;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, BtaEvtDeviceAdd)
#pragma alloc_text(PAGE, BtaEvtPrepareHardware)
#pragma alloc_text(PAGE, BtaEvtReleaseHardware)
#endif

/* ------------------------------------------------------------------------------------------- */
/* BRB helpers                                                                                  */

static NTSTATUS BtaSubmitBrbSync(PBTA_DEVICE_CONTEXT ctx, PBRB brb, ULONG brbSize)
{
    WDF_MEMORY_DESCRIPTOR arg1;
    WDF_REQUEST_SEND_OPTIONS opts;
    PAGED_CODE();
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&arg1, brb, brbSize);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_SEC(15));
    return WdfIoTargetSendInternalIoctlOthersSynchronously(ctx->IoTarget, NULL, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &arg1, NULL,
                                                           NULL, &opts, NULL);
}

static PBTA_CHANNEL BtaFindChannel(PBTA_DEVICE_CONTEXT ctx, ULONG id)
{
    ULONG i;
    for (i = 0; i < BTA2DP_MAX_CHANNELS; ++i)
        if (ctx->Channels[i].InUse && ctx->Channels[i].Id == id) return &ctx->Channels[i];
    return NULL;
}

/* Indication callback registered with the L2CAP channel (remote disconnect etc.). */
static VOID BtaIndication(_In_ PVOID context, _In_ INDICATION_CODE code, _In_ PINDICATION_PARAMETERS params)
{
    PBTA_CHANNEL ch = (PBTA_CHANNEL)context;
    UNREFERENCED_PARAMETER(params);
    if (code == IndicationRemoteDisconnect) {
        /* Pending ACL transfers on the channel are completed by the stack with an error. */
        ch->Open = FALSE;
        BtaLog(DPFLTR_INFO_LEVEL, "channel %lu: remote disconnect", ch->Id);
    }
}

static VOID BtaFillInfo(PBTA_CHANNEL ch, BTA2DP_CHANNEL_INFO* info)
{
    RtlZeroMemory(info, sizeof *info);
    info->ChannelId = ch->Id;
    info->OutMtu = ch->OutMtu;
    info->InMtu = ch->InMtu;
    info->PendingSends = (ULONG)ch->PendingSends;
    info->State = ch->Open ? BTA2DP_CHANNEL_STATE_OPEN : BTA2DP_CHANNEL_STATE_CLOSED;
    info->BytesSent = ch->BytesSent;
    info->BytesReceived = ch->BytesReceived;
}

/* ------------------------------------------------------------------------------------------- */
/* Channel open / close (PASSIVE_LEVEL, called from the sequential default queue)               */

static NTSTATUS BtaOpenChannel(PBTA_DEVICE_CONTEXT ctx, const BTA2DP_OPEN_REQUEST* req, BTA2DP_CHANNEL_INFO* out)
{
    NTSTATUS status;
    PBTA_CHANNEL ch = NULL;
    PBRB brb;
    struct _BRB_L2CA_OPEN_CHANNEL* oc;
    ULONG i;

    if (req->Psm != BTA_PSM_AVDTP) return STATUS_INVALID_PARAMETER;  /* contract v1: AVDTP only */
    if (req->InMtu < BTA_MIN_MTU) return STATUS_INVALID_PARAMETER;
    if (!ctx->ProfileValid) return STATUS_DEVICE_NOT_READY;

    WdfWaitLockAcquire(ctx->ChannelLock, NULL);
    for (i = 0; i < BTA2DP_MAX_CHANNELS; ++i)
        if (!ctx->Channels[i].InUse) {
            ch = &ctx->Channels[i];
            RtlZeroMemory(ch, sizeof *ch);
            ch->InUse = TRUE;
            ch->Id = ++ctx->NextChannelId;
            ch->Device = ctx;
            break;
        }
    WdfWaitLockRelease(ctx->ChannelLock);
    if (!ch) return STATUS_TOO_MANY_SESSIONS;

    brb = ctx->Profile.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL, BTA_POOL_TAG);
    if (!brb) {
        ch->InUse = FALSE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    oc = &brb->BrbL2caOpenChannel;
    oc->BtAddress = ctx->RemoteAddress;
    oc->Psm = req->Psm;
    oc->ChannelFlags = CF_ROLE_EITHER;
    oc->ConfigOut.Flags = CFG_MTU;
    oc->ConfigOut.Mtu.Max = req->InMtu;
    oc->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    oc->ConfigOut.Mtu.Preferred = req->InMtu;
    oc->ConfigIn.Flags = CFG_MTU;
    oc->ConfigIn.Mtu.Max = 0xFFFF;
    oc->ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
    oc->ConfigIn.Mtu.Preferred = 1024;
    oc->CallbackFlags = CALLBACK_DISCONNECT;
    oc->Callback = BtaIndication;
    oc->CallbackContext = ch;
    oc->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(ctx->Device);
    oc->IncomingQueueDepth = 32;

    status = BtaSubmitBrbSync(ctx, brb, sizeof(*oc));
    if (NT_SUCCESS(status)) {
        ch->Handle = oc->ChannelHandle;
        ch->OutMtu = oc->OutResults.Params.Mtu;
        ch->InMtu = oc->InResults.Params.Mtu;
        ch->Open = TRUE;
        BtaFillInfo(ch, out);
        BtaLog(DPFLTR_INFO_LEVEL, "channel %lu open psm=0x%04x outMtu=%u inMtu=%u", ch->Id, req->Psm, ch->OutMtu, ch->InMtu);
    } else {
        BtaLog(DPFLTR_WARNING_LEVEL, "open psm=0x%04x failed 0x%08x", req->Psm, status);
        ch->InUse = FALSE;
    }
    ctx->Profile.BthFreeBrb(brb);
    return status;
}

static NTSTATUS BtaCloseChannelLocked(PBTA_DEVICE_CONTEXT ctx, PBTA_CHANNEL ch)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (ch->Open || ch->Handle) {
        PBRB brb = ctx->Profile.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL, BTA_POOL_TAG);
        if (brb) {
            brb->BrbL2caCloseChannel.BtAddress = ctx->RemoteAddress;
            brb->BrbL2caCloseChannel.ChannelHandle = ch->Handle;
            status = BtaSubmitBrbSync(ctx, brb, sizeof(brb->BrbL2caCloseChannel));
            ctx->Profile.BthFreeBrb(brb);
        } else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    ch->Open = FALSE;
    ch->Handle = NULL;
    ch->InUse = FALSE;
    return status;
}

/* ------------------------------------------------------------------------------------------- */
/* Data transfer (asynchronous child requests carrying ACL transfer BRBs)                        */

VOID BtaTransferComplete(_In_ WDFREQUEST child, _In_ WDFIOTARGET target, _In_ PWDF_REQUEST_COMPLETION_PARAMS params,
                         _In_ WDFCONTEXT context)
{
    PBTA_TRANSFER t = (PBTA_TRANSFER)context;
    PBTA_DEVICE_CONTEXT ctx = t->Channel->Device;
    NTSTATUS status = params->IoStatus.Status;
    struct _BRB_L2CA_ACL_TRANSFER* xfer = &t->Brb->BrbL2caAclTransfer;
    ULONG_PTR bytes = 0;
    KIRQL irql;
    UNREFERENCED_PARAMETER(target);

    if (NT_SUCCESS(status)) {
        /* For IN transfers RemainingBufferSize holds the unused space. */
        bytes = t->Outbound ? xfer->BufferSize : (xfer->BufferSize - xfer->RemainingBufferSize);
    }
    KeAcquireSpinLock(&ctx->StatsLock, &irql);
    if (t->Outbound) {
        t->Channel->PendingSends--;
        if (NT_SUCCESS(status)) t->Channel->BytesSent += bytes;
    } else if (NT_SUCCESS(status)) {
        t->Channel->BytesReceived += bytes;
    }
    KeReleaseSpinLock(&ctx->StatsLock, irql);

    /* If the original was cancelled, BtaEvtOriginalCancel completes it (and drops its reference). */
    if (WdfRequestUnmarkCancelable(t->Original) != STATUS_CANCELLED) {
        /* Remote close is reported to user mode as a zero-length successful receive. */
        if (!t->Outbound && (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_REMOTE_DISCONNECT)) {
            status = STATUS_SUCCESS;
            bytes = 0;
        }
        WdfRequestCompleteWithInformation(t->Original, status, bytes);
        WdfObjectDereference(child);  /* reference taken for the cancel path */
    }
    ctx->Profile.BthFreeBrb(t->Brb);
    WdfObjectDelete(child);  /* deferred until the last reference is dropped */
}

VOID BtaEvtOriginalCancel(_In_ WDFREQUEST original)
{
    PBTA_ORIGINAL o = BtaGetOriginal(original);
    WDFREQUEST child = o->Child;
    WdfRequestCancelSentRequest(child);  /* child completes later with STATUS_CANCELLED */
    WdfObjectDereference(child);
    WdfRequestComplete(original, STATUS_CANCELLED);
}

static NTSTATUS BtaStartTransfer(PBTA_DEVICE_CONTEXT ctx, WDFREQUEST original, PBTA_CHANNEL ch, PMDL mdl, ULONG length,
                                 BOOLEAN outbound)
{
    NTSTATUS status;
    WDFREQUEST child = NULL;
    WDF_OBJECT_ATTRIBUTES attr;
    PBTA_TRANSFER t;
    PBTA_ORIGINAL o = NULL;
    WDFMEMORY brbMem = NULL;
    struct _BRB_L2CA_ACL_TRANSFER* xfer;
    PBRB brb;
    KIRQL irql;

    if (!ch->Open) return STATUS_DEVICE_NOT_CONNECTED;
    if (outbound && length > ch->OutMtu) return STATUS_INVALID_BUFFER_SIZE;

    brb = ctx->Profile.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BTA_POOL_TAG);
    if (!brb) return STATUS_INSUFFICIENT_RESOURCES;
    xfer = &brb->BrbL2caAclTransfer;
    xfer->BtAddress = ctx->RemoteAddress;
    xfer->ChannelHandle = ch->Handle;
    xfer->TransferFlags = outbound ? ACL_TRANSFER_DIRECTION_OUT : (ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK);
    xfer->BufferMDL = mdl;
    xfer->Buffer = NULL;
    xfer->BufferSize = length;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BTA_TRANSFER);
    attr.ParentObject = ctx->Device;
    status = WdfRequestCreate(&attr, ctx->IoTarget, &child);
    if (!NT_SUCCESS(status)) {
        ctx->Profile.BthFreeBrb(brb);
        return status;
    }
    t = BtaGetTransfer(child);
    t->Brb = brb;
    t->Original = original;
    t->Channel = ch;
    t->Outbound = outbound;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = child;  /* freed together with the child request */
    status = WdfMemoryCreatePreallocated(&attr, brb, sizeof(*xfer), &brbMem);
    if (NT_SUCCESS(status))
        status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, child, IOCTL_INTERNAL_BTH_SUBMIT_BRB, brbMem,
                                                                NULL, NULL, NULL, NULL, NULL);
    if (NT_SUCCESS(status)) {
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BTA_ORIGINAL);
        status = WdfObjectAllocateContext(original, &attr, (PVOID*)&o);
    }
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(child);
        ctx->Profile.BthFreeBrb(brb);
        return status;
    }
    o->Child = child;
    WdfObjectReference(child);  /* released by the cancel routine or the completion routine */
    WdfRequestSetCompletionRoutine(child, BtaTransferComplete, t);

    status = WdfRequestMarkCancelableEx(original, BtaEvtOriginalCancel);
    if (!NT_SUCCESS(status)) {  /* already cancelled by the caller */
        WdfObjectDereference(child);
        WdfObjectDelete(child);
        ctx->Profile.BthFreeBrb(brb);
        return status;
    }
    if (outbound) {
        KeAcquireSpinLock(&ctx->StatsLock, &irql);
        ch->PendingSends++;
        KeReleaseSpinLock(&ctx->StatsLock, irql);
    }
    if (!WdfRequestSend(child, ctx->IoTarget, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(child);
        if (outbound) {
            KeAcquireSpinLock(&ctx->StatsLock, &irql);
            ch->PendingSends--;
            KeReleaseSpinLock(&ctx->StatsLock, irql);
        }
        if (WdfRequestUnmarkCancelable(original) == STATUS_CANCELLED) {
            /* Cancel routine owns completion of the original and drops the reference. */
            WdfObjectDelete(child);
            ctx->Profile.BthFreeBrb(brb);
            return STATUS_PENDING;
        }
        WdfObjectDereference(child);
        WdfObjectDelete(child);
        ctx->Profile.BthFreeBrb(brb);
        return status;
    }
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------------------------------- */
/* IOCTL dispatch                                                                               */

VOID BtaEvtIoDeviceControl(_In_ WDFQUEUE queue, _In_ WDFREQUEST request, _In_ size_t outLen, _In_ size_t inLen, _In_ ULONG code)
{
    PBTA_DEVICE_CONTEXT ctx = BtaGetDeviceContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    PVOID in = NULL, out = NULL;
    UNREFERENCED_PARAMETER(outLen);
    UNREFERENCED_PARAMETER(inLen);

    switch (code) {
    case IOCTL_BTA2DP_GET_INFO: {
        BTA2DP_DEVICE_INFO* di;
        ULONG i, open = 0;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(BTA2DP_DEVICE_INFO), &out, NULL);
        if (!NT_SUCCESS(status)) break;
        di = (BTA2DP_DEVICE_INFO*)out;
        RtlZeroMemory(di, sizeof *di);
        di->Version = BTA2DP_IOCTL_VERSION;
        di->RemoteAddress = ctx->RemoteAddress;
        for (i = 0; i < BTA2DP_MAX_CHANNELS; ++i)
            if (ctx->Channels[i].InUse && ctx->Channels[i].Open) ++open;
        di->OpenChannels = open;
        di->Flags = open ? BTA2DP_INFO_FLAG_REMOTE_CONNECTED : 0;
        info = sizeof *di;
        break;
    }
    case IOCTL_BTA2DP_OPEN_CHANNEL: {
        status = WdfRequestRetrieveInputBuffer(request, sizeof(BTA2DP_OPEN_REQUEST), &in, NULL);
        if (!NT_SUCCESS(status)) break;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(BTA2DP_CHANNEL_INFO), &out, NULL);
        if (!NT_SUCCESS(status)) break;
        {
            BTA2DP_OPEN_REQUEST req = *(BTA2DP_OPEN_REQUEST*)in;  /* copy before reuse of system buffer */
            BTA2DP_CHANNEL_INFO ci;
            status = BtaOpenChannel(ctx, &req, &ci);
            if (NT_SUCCESS(status)) {
                *(BTA2DP_CHANNEL_INFO*)out = ci;
                info = sizeof ci;
            }
        }
        break;
    }
    case IOCTL_BTA2DP_ACCEPT_CHANNEL:
        status = STATUS_NOT_SUPPORTED;  /* v1: source always initiates */
        break;
    case IOCTL_BTA2DP_CLOSE_CHANNEL:
    case IOCTL_BTA2DP_QUERY_CHANNEL: {
        BTA2DP_CHANNEL_REF ref;
        PBTA_CHANNEL ch;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(BTA2DP_CHANNEL_REF), &in, NULL);
        if (!NT_SUCCESS(status)) break;
        ref = *(BTA2DP_CHANNEL_REF*)in;
        WdfWaitLockAcquire(ctx->ChannelLock, NULL);
        ch = BtaFindChannel(ctx, ref.ChannelId);
        if (!ch) {
            status = STATUS_INVALID_HANDLE;
        } else if (code == IOCTL_BTA2DP_CLOSE_CHANNEL) {
            status = BtaCloseChannelLocked(ctx, ch);
        } else {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(BTA2DP_CHANNEL_INFO), &out, NULL);
            if (NT_SUCCESS(status)) {
                BtaFillInfo(ch, (BTA2DP_CHANNEL_INFO*)out);
                info = sizeof(BTA2DP_CHANNEL_INFO);
            }
        }
        WdfWaitLockRelease(ctx->ChannelLock);
        break;
    }
    case IOCTL_BTA2DP_SEND:
    case IOCTL_BTA2DP_RECEIVE: {
        BTA2DP_CHANNEL_REF ref;
        PBTA_CHANNEL ch;
        PMDL mdl = NULL;
        size_t len = 0;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(BTA2DP_CHANNEL_REF), &in, NULL);
        if (!NT_SUCCESS(status)) break;
        ref = *(BTA2DP_CHANNEL_REF*)in;
        status = WdfRequestRetrieveOutputWdmMdl(request, &mdl);
        if (!NT_SUCCESS(status)) break;
        len = MmGetMdlByteCount(mdl);
        if (len == 0 || len > 0xFFFF) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        WdfWaitLockAcquire(ctx->ChannelLock, NULL);
        ch = BtaFindChannel(ctx, ref.ChannelId);
        status = ch ? BtaStartTransfer(ctx, request, ch, mdl, (ULONG)len, code == IOCTL_BTA2DP_SEND) : STATUS_INVALID_HANDLE;
        WdfWaitLockRelease(ctx->ChannelLock);
        if (status == STATUS_PENDING) return;  /* completed by BtaTransferComplete / cancel */
        break;
    }
    default: break;
    }
    WdfRequestCompleteWithInformation(request, status, info);
}

/* ------------------------------------------------------------------------------------------- */
/* PnP                                                                                          */

NTSTATUS BtaEvtPrepareHardware(_In_ WDFDEVICE device, _In_ WDFCMRESLIST raw, _In_ WDFCMRESLIST translated)
{
    PBTA_DEVICE_CONTEXT ctx = BtaGetDeviceContext(device);
    NTSTATUS status;
    BTH_DEVICE_INFO devInfo;
    WDF_MEMORY_DESCRIPTOR outDesc;
    UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();

    status = WdfFdoQueryForInterface(device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE, (PINTERFACE)&ctx->Profile,
                                     sizeof(ctx->Profile), BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status)) {
        BtaLog(DPFLTR_ERROR_LEVEL, "QueryForInterface(BTHDDI profile) failed 0x%08x", status);
        return status;
    }
    ctx->ProfileValid = TRUE;

    /* The BTHENUM PDO describes the remote device this devnode represents. */
    RtlZeroMemory(&devInfo, sizeof devInfo);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, &devInfo, sizeof devInfo);
    status = WdfIoTargetSendInternalIoctlSynchronously(ctx->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &outDesc,
                                                       NULL, NULL);
    if (!NT_SUCCESS(status)) {
        BtaLog(DPFLTR_ERROR_LEVEL, "IOCTL_INTERNAL_BTHENUM_GET_DEVINFO failed 0x%08x", status);
        return status;
    }
    ctx->RemoteAddress = devInfo.address;
    BtaLog(DPFLTR_INFO_LEVEL, "bound to remote %012I64X", ctx->RemoteAddress);
    return STATUS_SUCCESS;
}

NTSTATUS BtaEvtReleaseHardware(_In_ WDFDEVICE device, _In_ WDFCMRESLIST translated)
{
    PBTA_DEVICE_CONTEXT ctx = BtaGetDeviceContext(device);
    ULONG i;
    UNREFERENCED_PARAMETER(translated);
    PAGED_CODE();
    WdfWaitLockAcquire(ctx->ChannelLock, NULL);
    for (i = 0; i < BTA2DP_MAX_CHANNELS; ++i)
        if (ctx->Channels[i].InUse) BtaCloseChannelLocked(ctx, &ctx->Channels[i]);
    WdfWaitLockRelease(ctx->ChannelLock);
    if (ctx->ProfileValid && ctx->Profile.Interface.InterfaceDereference)
        ctx->Profile.Interface.InterfaceDereference(ctx->Profile.Interface.Context);
    ctx->ProfileValid = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS BtaEvtDeviceAdd(_In_ WDFDRIVER driver, _Inout_ PWDFDEVICE_INIT init)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG qcfg;
    PBTA_DEVICE_CONTEXT ctx;
    UNREFERENCED_PARAMETER(driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BtaEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = BtaEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WdfDeviceInitSetIoType(init, WdfDeviceIoDirect);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BTA_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) return status;
    ctx = BtaGetDeviceContext(device);
    ctx->Device = device;
    ctx->IoTarget = WdfDeviceGetIoTarget(device);
    KeInitializeSpinLock(&ctx->StatsLock);
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->ChannelLock);
    if (!NT_SUCCESS(status)) return status;

    /* Parallel queue: RECEIVE requests pend for long periods and must not block SEND/QUERY. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = BtaEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_BTA2DP_L2CAP, NULL);
    return status;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, BtaEvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
