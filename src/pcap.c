/* GPL-3.0-or-later. Radiotap layout follows https://www.radiotap.org/.
 * All multibyte reads use bytes, so files need no native alignment or byte order.
 */
#include "pcap.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough for normal radiotap plus the complete 802.11 MAC header. Larger
 * radiotap headers are counted as unsupported; no packet-sized allocations.
 */
#define PACKET_PREFIX 4096u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p, int big)
{
    if (big) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                    (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
           (uint32_t)p[1] << 8 | p[0];
}

static uint16_t u16(const uint8_t *p, int big)
{
    return big ? (uint16_t)((uint16_t)p[0] << 8 | p[1]) : le16(p);
}

static void errorf(char *error, size_t size, const char *fmt, ...)
{
    va_list args;
    if (!error || !size) return;
    va_start(args, fmt);
    vsnprintf(error, size, fmt, args);
    va_end(args);
}

static size_t align_offset(size_t offset, unsigned align)
{
    return (offset + align - 1) & ~(size_t)(align - 1);
}

static void radio_value(struct packet_record *record, unsigned index,
                        const uint8_t *value, size_t size, uint8_t *flags)
{
    switch (index) {
    case 1: if (size >= 1) *flags = value[0]; break;
    case 2: if (size >= 1) record->rate_half = value[0]; break;
    case 3: if (size >= 2) record->frequency = le16(value); break;
    case 5:
        if (size >= 1) record->signal = value[0] < 128 ? value[0] : value[0] - 256;
        break;
    case 19:
        if (size >= 3 && (value[0] & 2)) record->mcs = value[2];
        break;
    }
}

/* Validate all presence words before looking at fields. Unknown vendor data is
 * skipped using its declared length. Unknown ordinary fields make this record
 * unsupported, since their implicit size cannot safely be guessed.
 */
static int radio_parse(const uint8_t *bytes, size_t available, size_t captured,
                       struct packet_record *record, size_t *header_length,
                       uint8_t *flags)
{
    static const uint8_t alignment[] = {
        8, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 1, 1, 1, 2, 2,
        1, 1, 4, 1, 4, 2, 8, 2, 2, 2, 1, 2
    };
    static const uint8_t length[] = {
        8, 1, 1, 4, 2, 1, 1, 2, 2, 2, 1, 1, 1, 1, 2, 2,
        1, 1, 8, 3, 8, 12, 12, 12, 12, 6, 1, 4
    };
    size_t total, data, word, words_end;
    unsigned base = 0;
    int vendor = 0;
    if (available < 8 || bytes[0] != 0) return -1;
    total = le16(bytes + 2);
    if (total < 8 || total > captured || total > available) return -1;
    words_end = 4;
    do {
        if (words_end + 4 > total) return -1;
        word = u32(bytes + words_end, 0);
        words_end += 4;
    } while (word & 0x80000000u);
    data = words_end;
    for (size_t mask_offset = 4; mask_offset < words_end; mask_offset += 4) {
        uint32_t mask = u32(bytes + mask_offset, 0);
        int reset = 0;
        if ((mask & 0x60000000u) == 0x60000000u) return -1;
        for (unsigned bit = 0; bit < 31; bit++) {
            unsigned index = base + bit;
            size_t size;
            if (!(mask & (1u << bit))) continue;
            if (bit == 29) {
                vendor = 0;
                reset = 1;
                continue;
            }
            if (bit == 30) {
                data = align_offset(data, 2);
                if (data + 6 > total) return -1;
                size = 6u + le16(bytes + data + 4);
                if (size > total - data) return -1;
                data += size;
                vendor = 1;
                reset = 1;
                continue;
            }
            if (vendor) continue; /* Vendor bytes were consumed by skip_length. */
            if (index == 28) {
                /* TLVs consume the remainder; higher presence bits forbidden. */
                if ((mask & 0xe0000000u) || mask_offset + 4 != words_end) return -1;
                data = align_offset(data, 4);
                if (data > total) return -1;
                while (data < total) {
                    uint16_t type;
                    if (total - data < 4) return -1;
                    type = le16(bytes + data);
                    size = le16(bytes + data + 2);
                    if (type == 29 || type == 31 || size > total - data - 4) return -1;
                    radio_value(record, type, bytes + data + 4, size, flags);
                    data += align_offset(size + 4, 4);
                    if (data > total) return -1;
                }
                continue;
            }
            if (index >= sizeof(length)) return -1;
            data = align_offset(data, alignment[index]);
            size = length[index];
            if (data > total || size > total - data) return -1;
            radio_value(record, index, bytes + data, size, flags);
            data += size;
        }
        base = reset ? 0 : base + 32;
    }
    *header_length = total;
    return 0;
}

static int packet_parse(const uint8_t *bytes, size_t available, uint32_t captured,
                        uint32_t original, struct packet_record *record)
{
    size_t radio_length, frame_length, saved, minimum;
    uint8_t flags = 0;
    uint16_t control;
    const uint8_t *frame;
    memset(record, 0, sizeof(*record));
    record->signal = PACKET_SIGNAL_UNKNOWN;
    record->sequence = PACKET_SEQUENCE_UNKNOWN;
    record->mcs = PACKET_MCS_UNKNOWN;
    record->type = 255;
    if (radio_parse(bytes, available, captured, record, &radio_length, &flags)) return -1;
    saved = available - radio_length;
    frame_length = captured - radio_length;
    record->length = original - radio_length;
    if (flags & 0x10) {
        /* FCS occupies the original tail, which need not be captured in full. */
        if (record->length < 4) return -1;
        record->length -= 4;
        if (captured == original) {
            if (frame_length < 4) return -1;
            frame_length -= 4;
        }
    }
    if (frame_length < 2 || saved < 2) return -1;
    frame = bytes + radio_length;
    control = le16(frame);
    if (control & 3) return -1; /* Unsupported 802.11 version. */
    record->type = (control >> 2) & 3;
    record->subtype = (control >> 4) & 15;
    record->protected_frame = !!(control & 0x4000);
    record->retry = !!(control & 0x0800);
    record->bad_fcs = !!(flags & 0x40);
    if (record->type == 0 || record->type == 2) {
        minimum = 24;
        if (record->type == 2) {
            if ((control & 0x0300) == 0x0300) minimum += 6;
            if (record->subtype & 8) {
                minimum += 2;
                if (control & 0x8000) minimum += 4;
            }
        }
        if (record->length < minimum || frame_length < minimum || saved < minimum) return -1;
        record->sequence = le16(frame + 22) >> 4;
    } else if (record->type == 1) {
        /* Common control headers; extension/trigger formats are unsupported. */
        if (record->subtype < 7) return -1;
        minimum = record->subtype == 12 || record->subtype == 13 ? 10 : 16;
        if (record->subtype == 8) minimum = 20;
        if (record->subtype == 9) minimum = 28;
        if (record->length < minimum || frame_length < minimum || saved < minimum) return -1;
    } else return -1;
    memcpy(record->receiver, frame + 4, 6);
    record->has_receiver = 1;
    if (record->type != 1 || (record->subtype != 7 && record->subtype != 12 && record->subtype != 13)) {
        memcpy(record->transmitter, frame + 10, 6);
        record->has_transmitter = 1;
    }
    return 0;
}

void pcap_free(struct packet_capture *capture)
{
    if (!capture) return;
    free(capture->packets);
    memset(capture, 0, sizeof(*capture));
}

int pcap_load(const char *path, struct packet_capture *capture,
              char *error, size_t error_size)
{
    FILE *file;
    uint8_t header[24], bytes[PACKET_PREFIX], discard[PACKET_PREFIX];
    uint32_t snaplen, scale;
    int big, nano;
    size_t capacity = 0, records = 0;
    uint32_t first_seconds = 0, first_fraction = 0;
    if (!capture) { errorf(error, error_size, "Missing capture destination"); return -1; }
    memset(capture, 0, sizeof(*capture));
    if (error && error_size) error[0] = 0;
    file = path ? fopen(path, "rb") : NULL;
    if (!file) { errorf(error, error_size, "Cannot open capture: %s", path ? strerror(errno) : "no path"); return -1; }
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        errorf(error, error_size, "Truncated PCAP header (need 24 bytes)");
        goto fail;
    }
    if (!memcmp(header, "\xd4\xc3\xb2\xa1", 4)) { big = 0; nano = 0; }
    else if (!memcmp(header, "\xa1\xb2\xc3\xd4", 4)) { big = 1; nano = 0; }
    else if (!memcmp(header, "\x4d\x3c\xb2\xa1", 4)) { big = 0; nano = 1; }
    else if (!memcmp(header, "\xa1\xb2\x3c\x4d", 4)) { big = 1; nano = 1; }
    else {
        errorf(error, error_size, !memcmp(header, "\x0a\x0d\x0d\x0a", 4) ?
               "PCAPNG is unsupported; export classic radiotap PCAP" : "Unrecognized PCAP magic");
        goto fail;
    }
    if (u16(header + 4, big) != 2 || u16(header + 6, big) != 4) {
        errorf(error, error_size, "Unsupported PCAP version (need 2.4)");
        goto fail;
    }
    snaplen = u32(header + 16, big);
    /* High bits hold standardized link-type flags; low 16 bits hold LINKTYPE. */
    if ((u32(header + 20, big) & 0xffff) != 127) {
        errorf(error, error_size, "Capture needs radiotap link type 127");
        goto fail;
    }
    if (!snaplen) { errorf(error, error_size, "Invalid PCAP snapshot length 0"); goto fail; }
    scale = nano ? 1000000000u : 1000000u;
    for (;;) {
        size_t read;
        uint32_t seconds, fraction, included, original, remaining;
        struct packet_record record;
        if (records == PCAP_MAX_PACKETS) {
            int extra = fgetc(file);
            if (extra != EOF) capture->limited = 1;
            if (ferror(file)) { errorf(error, error_size, "Cannot read capture"); goto fail; }
            break;
        }
        read = fread(header, 1, 16, file);
        if (!read && !ferror(file)) break;
        records++;
        if (read != 16) { errorf(error, error_size, "Truncated PCAP record %zu header", records); goto fail; }
        seconds = u32(header, big);
        fraction = u32(header + 4, big);
        included = u32(header + 8, big);
        original = u32(header + 12, big);
        if (fraction >= scale || included > snaplen || included > original) {
            errorf(error, error_size, "Invalid PCAP record %zu timestamp or lengths", records);
            goto fail;
        }
        read = included < sizeof(bytes) ? included : sizeof(bytes);
        if (fread(bytes, 1, read, file) != read) {
            errorf(error, error_size, "Truncated PCAP record %zu data", records);
            goto fail;
        }
        remaining = included - read;
        while (remaining) {
            size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (fread(discard, 1, chunk, file) != chunk) {
                errorf(error, error_size, "Truncated PCAP record %zu data", records);
                goto fail;
            }
            remaining -= chunk;
        }
        if (packet_parse(bytes, read, included, original, &record)) {
            capture->skipped++;
            continue;
        }
        if (capture->count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 1024;
            struct packet_record *new_packets;
            if (new_capacity > PCAP_MAX_PACKETS) new_capacity = PCAP_MAX_PACKETS;
            new_packets = realloc(capture->packets, new_capacity * sizeof(*new_packets));
            if (!new_packets) { errorf(error, error_size, "Out of memory loading capture"); goto fail; }
            capture->packets = new_packets;
            capacity = new_capacity;
        }
        if (!capture->count) {
            first_seconds = seconds;
            first_fraction = fraction;
        }
        record.time = (double)seconds - first_seconds +
                      ((double)fraction - first_fraction) / scale;
        if (record.time < capture->duration) record.time = capture->duration;
        capture->duration = record.time;
        capture->packets[capture->count++] = record;
    }
    if (!capture->count) {
        errorf(error, error_size, "No supported 802.11 packets in capture (%zu skipped)", capture->skipped);
        goto fail;
    }
    fclose(file);
    return 0;
fail:
    fclose(file);
    pcap_free(capture);
    return -1;
}

const char *packet_type_name(const struct packet_record *packet)
{
    static const char *management[16] = {
        "Assoc req", "Assoc resp", "Reassoc req", "Reassoc resp", "Probe req", "Probe resp",
        "Mgmt 6", "Mgmt 7", "Beacon", "ATIM", "Disassoc", "Auth", "Deauth", "Action",
        "Action ACK", "Mgmt 15"
    };
    static const char *control[16] = {
        "Control 0", "Control 1", "Trigger", "Control 3", "Control 4", "Control 5", "Control 6",
        "Wrapper", "Block ACK req", "Block ACK", "PS-Poll", "RTS", "CTS", "ACK", "CF-End", "CF-End ACK"
    };
    static const char *data[16] = {
        "Data", "Data CF-ACK", "Data CF-Poll", "Data CF-Both", "Null", "CF-ACK", "CF-Poll", "CF-Both",
        "QoS data", "QoS CF-ACK", "QoS CF-Poll", "QoS CF-Both", "QoS null", "Data 13", "QoS poll", "QoS both"
    };
    if (!packet || packet->subtype > 15) return "Unknown";
    switch (packet->type) {
    case 0: return management[packet->subtype];
    case 1: return control[packet->subtype];
    case 2: return data[packet->subtype];
    default: return "Unknown";
    }
}
