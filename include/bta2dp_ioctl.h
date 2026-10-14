/*
 * BtA2dpL2cap.sys <-> user-mode contract (shared by the KMDF driver and bta_platform).
 *
 * The driver is a thin L2CAP proxy bound to one remote device's BTHENUM\{0000110B-...} devnode.
 * It owns no AVDTP/codec logic: user mode (bta2dp-service) runs AVDTP signalling, codecs and RTP.
 *
 * Versioning: BTA2DP_IOCTL_VERSION increments on any incompatible change; GET_INFO reports it.
 * All structures are naturally aligned, little-endian, fixed size.
 */
#ifndef BTA2DP_IOCTL_H
#define BTA2DP_IOCTL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif
#include <guiddef.h>

#define BTA2DP_IOCTL_VERSION 1

/* {6B0E4A21-3F57-4C2A-9C1E-5D2B7A9F0B11} device interface registered per bound remote device. */
DEFINE_GUID(GUID_DEVINTERFACE_BTA2DP_L2CAP, 0x6b0e4a21, 0x3f57, 0x4c2a, 0x9c, 0x1e, 0x5d, 0x2b, 0x7a, 0x9f, 0x0b, 0x11);

#define FILE_DEVICE_BTA2DP 0x8A2D

#define BTA2DP_CTL(fn, method) ((ULONG)CTL_CODE(FILE_DEVICE_BTA2DP, 0x800 + (fn), method, FILE_READ_DATA | FILE_WRITE_DATA))

/* out: BTA2DP_DEVICE_INFO */
#define IOCTL_BTA2DP_GET_INFO        BTA2DP_CTL(0, METHOD_BUFFERED)
/* in: BTA2DP_OPEN_REQUEST  out: BTA2DP_CHANNEL_INFO. Connects an outgoing L2CAP channel. */
#define IOCTL_BTA2DP_OPEN_CHANNEL    BTA2DP_CTL(1, METHOD_BUFFERED)
/* in: BTA2DP_ACCEPT_REQUEST out: BTA2DP_CHANNEL_INFO. Waits for a remote-initiated channel. */
#define IOCTL_BTA2DP_ACCEPT_CHANNEL  BTA2DP_CTL(2, METHOD_BUFFERED)
/* in: BTA2DP_CHANNEL_REF */
#define IOCTL_BTA2DP_CLOSE_CHANNEL   BTA2DP_CTL(3, METHOD_BUFFERED)
/* in: BTA2DP_CHANNEL_REF (system buffer); out buffer (MDL, read by driver) = SDU to send. */
#define IOCTL_BTA2DP_SEND            BTA2DP_CTL(4, METHOD_IN_DIRECT)
/* in: BTA2DP_CHANNEL_REF; out buffer (MDL) receives one SDU. Pends until data/close/cancel. */
#define IOCTL_BTA2DP_RECEIVE         BTA2DP_CTL(5, METHOD_OUT_DIRECT)
/* in: BTA2DP_CHANNEL_REF  out: BTA2DP_CHANNEL_INFO (includes pending send count) */
#define IOCTL_BTA2DP_QUERY_CHANNEL   BTA2DP_CTL(6, METHOD_BUFFERED)

#define BTA2DP_MAX_CHANNELS 4

#pragma pack(push, 8)

typedef struct _BTA2DP_DEVICE_INFO {
    ULONG Version;            /* BTA2DP_IOCTL_VERSION */
    ULONG Flags;              /* BTA2DP_INFO_FLAG_* */
    ULONGLONG RemoteAddress;  /* BTH_ADDR of the bound sink */
    ULONG OpenChannels;
    ULONG Reserved;
} BTA2DP_DEVICE_INFO;

#define BTA2DP_INFO_FLAG_REMOTE_CONNECTED 0x1

typedef struct _BTA2DP_OPEN_REQUEST {
    USHORT Psm;               /* must be 0x0019 (AVDTP) in version 1 */
    USHORT InMtu;             /* MTU we accept (48..65535) */
    USHORT FlushTimeoutMs;    /* 0 = infinite */
    USHORT Reserved;
    ULONG TimeoutMs;          /* connect timeout */
} BTA2DP_OPEN_REQUEST;

typedef struct _BTA2DP_ACCEPT_REQUEST {
    USHORT Psm;
    USHORT InMtu;
    ULONG TimeoutMs;
} BTA2DP_ACCEPT_REQUEST;

typedef struct _BTA2DP_CHANNEL_REF {
    ULONG ChannelId;
    ULONG Reserved;
} BTA2DP_CHANNEL_REF;

typedef struct _BTA2DP_CHANNEL_INFO {
    ULONG ChannelId;
    USHORT OutMtu;            /* negotiated MTU of the remote side (max SDU we may send) */
    USHORT InMtu;
    ULONG PendingSends;
    ULONG State;              /* BTA2DP_CHANNEL_STATE_* */
    ULONGLONG BytesSent;
    ULONGLONG BytesReceived;
} BTA2DP_CHANNEL_INFO;

#define BTA2DP_CHANNEL_STATE_OPEN 1
#define BTA2DP_CHANNEL_STATE_CLOSED 2

#pragma pack(pop)

#endif /* BTA2DP_IOCTL_H */
