/* Bounded, receive-only metadata loader for classic radiotap PCAP files.
 * GPL-3.0-or-later. Payloads are never retained or transmitted.
 */
#ifndef MIYOOSDR_PCAP_H
#define MIYOOSDR_PCAP_H

#include <stddef.h>
#include <stdint.h>

#define PCAP_MAX_PACKETS 100000u
#define PCAP_CLOCK_STEP 1.0          /* Seconds back in time that count as a clock reset. */
#define PACKET_SIGNAL_UNKNOWN 32767
#define PACKET_SEQUENCE_UNKNOWN 65535u
#define PACKET_MCS_UNKNOWN 255u

struct packet_record {
    double time;                 /* Seconds since first accepted record, never decreasing. */
    uint32_t length;              /* Original 802.11 length, excluding radiotap and FCS. */
    uint16_t frequency, rate_half, sequence;
    int16_t signal;               /* Signed driver dBm value, or PACKET_SIGNAL_UNKNOWN. */
    uint8_t type, subtype, protected_frame, retry, bad_fcs, mcs;
    uint8_t receiver[6], transmitter[6], has_receiver, has_transmitter;
};

struct packet_capture {
    struct packet_record *packets;
    size_t count;
    double duration;
    size_t skipped;
    int limited;                 /* Stopped after PCAP_MAX_PACKETS on-disk records. */
};

/* Returns 0 on success, -1 with a human-readable error on failure. On failure
 * capture is empty. Call with a fresh or previously freed capture. Unsupported
 * or malformed individual radiotap/802.11 headers increment skipped. Malformed
 * PCAP containers and truncated files fail, rather than presenting partial data.
 * PCAPNG and non-radiotap link types are intentionally unsupported.
 */
int pcap_load(const char *path, struct packet_capture *capture,
              char *error, size_t error_size);
void pcap_free(struct packet_capture *capture);
const char *packet_type_name(const struct packet_record *packet);

#endif
