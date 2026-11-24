/*
 * This is a "Manager" for the Nortel Millennium payhone.
 *
 * It can provision a Nortel Millennium payphone with Rev 1.0 or 1.3
 * Control PCP.  CDRs, Alarms, and Maintenance Reports can also be
 * retieved.
 *
 * www.github.com/hharte/mm_manager
 *
 * Copyright (c) 2020-2023, Howard M. Harte
 */

#include <stdio.h>  /* Standard input/output definitions */
#include <stdlib.h>
#include <stdint.h>
#include <string.h> /* String function definitions */
#include <time.h>
#include <stddef.h>
#ifdef _WIN32
# include <windows.h>
#else  /* ifdef _WIN32 */
#define __USE_BSD
# include <termios.h> /* POSIX terminal control definitions */
# include <unistd.h>
#endif /* _WIN32 */

#include "mm_manager.h"
#include "mm_serial.h"
#include "mm_udp.h"

static pkt_status_t receive_mm_packet(mm_proto_t* proto, mm_packet_t* pkt);
static pkt_status_t send_mm_packet(mm_proto_t* proto, uint8_t* payload, size_t len, uint8_t flags);
static pkt_status_t wait_for_mm_ack(mm_proto_t* proto);
static pkt_status_t send_mm_ack(mm_proto_t* proto, uint8_t flags);

extern volatile int inject_comm_error;

#define L2_STATE_SEARCH_FOR_START   1
#define L2_STATE_GET_FLAGS          2
#define L2_STATE_GET_LENGTH         3
#define L2_STATE_ACCUMULATE_DATA    4
#define L2_STATE_GET_CRC0           5
#define L2_STATE_GET_CRC1           6
#define L2_STATE_SEARCH_FOR_STOP    7


int proto_connect(mm_proto_t* proto) {
    proto->tx_seq = 0;
    proto->connected = 1;

    return (0);
}

int proto_disconnect(mm_proto_t *proto) {
    hangup_modem(proto->serial_context);
    proto->tx_seq = 0;
    proto->connected = 0;

    return (0);
}

int proto_connected(mm_proto_t* proto) {
    return (proto->connected);
}

int receive_mm_table(mm_proto_t* proto, mm_table_t* table) {
    mm_packet_t* pkt = &table->pkt;
    int status;

    status = receive_mm_packet(proto, pkt);

    if ((status != PKT_SUCCESS) && (status != PKT_ERROR_RETRY)) {
        if (proto->debuglevel > 2) print_mm_packet(RX, pkt);

        if (proto->connected) {
            send_mm_ack(proto, FLAG_NACK);  /* Retry unless the terminal disconnected. */
            status = wait_for_mm_ack(proto);
            if (status != PKT_ERROR_NACK) {
                fprintf(stderr, "%s: Expected NACK from terminal, status=0x%02x\n", __func__, status);
            }
        }
        return status;
    }

    proto->rx_seq = pkt->hdr.flags & FLAG_SEQUENCE;

    if (pkt->payload_len < PKT_TABLE_ID_OFFSET) {
        table->table_id = 0;
        print_mm_packet(RX, pkt);
        fprintf(stderr, "Error: Received an ACK without expecting it!\n");
        return 0;
    }

    phone_num_to_string(proto->terminal_id, sizeof(proto->terminal_id), pkt->payload, PKT_TABLE_ID_OFFSET);

    if (proto->debuglevel > 0) {
        print_mm_packet(RX, pkt);
    }

    /* Acknowledge the received packet */
    send_mm_ack(proto, FLAG_ACK);

    return (status);
}

int send_mm_table(mm_proto_t* proto, uint8_t* payload, size_t len) {
    size_t   bytes_remaining;
    size_t   chunk_len;
    pkt_status_t status = PKT_SUCCESS;
    uint8_t* p = payload;
    uint8_t  table_id;

    table_id = payload[0];
    bytes_remaining = len;
    printf("\tSending Table ID %d (0x%02x) %s...\n", table_id, table_id, table_to_string(table_id));

    while (bytes_remaining > 0) {
        if (bytes_remaining > PKT_TABLE_DATA_LEN_MAX) {
            chunk_len = PKT_TABLE_DATA_LEN_MAX;
        }
        else {
            chunk_len = bytes_remaining;
        }

        status = send_mm_packet(proto, p, chunk_len, 0);

        if (status != PKT_SUCCESS) break;

        p += chunk_len;
        bytes_remaining -= chunk_len;
        printf("\tTable %d (0x%02x) %s progress: (%3d%%) - %4d / %4zu\n",
            table_id, table_id, table_to_string(table_id),
            (uint16_t)(((p - payload) * 100) / len), (uint16_t)(p - payload), len);
    }

    return status;
}

int wait_for_table_ack(mm_proto_t* proto, uint8_t table_id) {
    mm_packet_t  packet = { { 0 }, { 0 }, { 0 }, 0, 0 };
    mm_packet_t* pkt = &packet;
    int status;
    int retries = 2;

    if (proto->debuglevel > 1) printf("Waiting for ACK for table %d (0x%02x)\n", table_id, table_id);

    while (retries > 0) {
        memset(pkt, 0, sizeof(mm_packet_t));
        status = receive_mm_packet(proto, pkt);

        if ((status == PKT_SUCCESS) || (status == PKT_ERROR_RETRY)) {
            proto->rx_seq = pkt->hdr.flags & FLAG_SEQUENCE;

            if (pkt->payload_len >= PKT_TABLE_ID_OFFSET) {
                phone_num_to_string(proto->terminal_id, sizeof(proto->terminal_id), pkt->payload, PKT_TABLE_ID_OFFSET);

                if (proto->debuglevel > 1) printf("Received packet from phone# %s\n", proto->terminal_id);

                if (proto->debuglevel > 2) print_mm_packet(RX, pkt);

                if ((pkt->payload[PKT_TABLE_ID_OFFSET] == DLOG_MT_TABLE_UPD_ACK) &&
                    (pkt->payload[PKT_TABLE_DATA_OFFSET] == table_id)) {
                    if (proto->debuglevel > 0) {
                        printf("Seq: %d: Received ACK for table %d (0x%02x)\n",
                            proto->rx_seq,
                            table_id,
                            table_id);
                    }
                    send_mm_ack(proto, FLAG_ACK);
                    return 0;
                }
                else {
                    printf("%s: Error: Received ACK for wrong table, expected %d (0x%02x), received %d (0x%02x)\n",
                        __func__, table_id, table_id, pkt->payload[6], pkt->payload[6]);
                    return -1;
                }
            }
        }
        else {
            printf("%s: ERROR: Did not receive ACK for table ID %d (0x%02x), status=%02x\n",
                __func__, table_id, table_id, status);

            if (proto->debuglevel > 2) print_mm_packet(RX, pkt);

            if (proto->connected) {
                send_mm_ack(proto, FLAG_RETRY);  /* Retry unless the terminal disconnected. */
                status = wait_for_mm_ack(proto);
                if (status != PKT_ERROR_NACK) {
                    fprintf(stderr, "%s: Expected NACK from terminal, status=0x%02x\n", __func__, status);
                }
                if (proto->connected) {
                    send_mm_ack(proto, FLAG_ACK);  /* Retry unless the terminal disconnected. */
                }
            }
            else {
                /* Not connected anymore, bail out. */
                break;
            }
        }
    }
    return status;
}


/*
 * Receive a packet from Millennium Terminal.
 *
 * Packets are formatted as follows:
 * +------+-------+--------+-----------+--------+-----+
 * |START | FLAGS | LENGTH | DATA .... | CRC-16 | END |
 * +------+-------+--------+-----------+--------+-----+
 */
static pkt_status_t receive_mm_packet(mm_proto_t *proto, mm_packet_t *pkt) {
    uint8_t pkt_received = 0;
    uint8_t databyte     = 0;
    uint8_t l2_state     = L2_STATE_SEARCH_FOR_START;
    pkt_status_t status  = PKT_SUCCESS;
    uint8_t timeout      = 0;

    pkt->payload_len = 0;
    memset(pkt, 0, sizeof(mm_packet_t));

    if (proto->monitor_carrier) {
        if ((serial_get_modem_status(proto->serial_context) & (MS_RING_ON | MS_RLSD_ON)) == 0) {
            fprintf(stderr, "%s: Carrier lost, bailing.\n", __func__);
            proto_disconnect(proto);
            return PKT_ERROR_NO_CARRIER;
        }
    }

    if (!proto->connected) {
        fprintf(stderr, "%s: Attempt to receive packet while disconnected, bailing.\n", __func__);
        return PKT_ERROR_DISCONNECT;
    }

    while (pkt_received == 0) {
        int inject_error = 0;
        ssize_t bytes_read;
        if (inject_comm_error == 1) {
            if (((proto->error_inject_type == ERROR_INJECT_CRC_DLOG_RX) && (proto->waiting_for_ack == 0)) ||
                ((proto->error_inject_type == ERROR_INJECT_CRC_ACK_RX)  && (proto->waiting_for_ack == 1))) {
                inject_error = (l2_state == L2_STATE_GET_CRC0) ? 1 : 0;
            }
        }
        if (inject_error == 1) {
            printf("Inject error type %d: Injecting error on READ now.\n", proto->error_inject_type);
            inject_comm_error = 0;
        }
        while ((bytes_read = read_serial(proto->serial_context, &databyte, 1, inject_error)) == 0) {
            if (!proto->connected) {
                return PKT_ERROR_DISCONNECT;
            }
            putchar('.');
            if (proto->monitor_carrier) {
                if ((serial_get_modem_status(proto->serial_context) & (MS_RING_ON | MS_RLSD_ON)) == 0) {
                    fprintf(stderr, "%s: Carrier lost, bailing.\n", __func__);
                    proto_disconnect(proto);
                    return PKT_ERROR_NO_CARRIER;
                }
            }

            fflush(stdout);
            timeout++;

            /*
             * A second of silence INSIDE a packet means the packet is broken.
             *
             * At 1200 baud a byte arrives every 8 ms, and a terminal sends a
             * packet's bytes back to back, so a whole read timeout with no
             * byte part-way through a frame is never a slow terminal. It is
             * a frame that lost bytes: an HT802 mutes 20-35 ms of the
             * terminal's audio when its DTMF detector false-triggers on the
             * 212A carrier, the modem drops a character or two, and the
             * STOP byte is read as CRC (or the length runs past the end).
             * Upstream then waited out PKT_TIMEOUT_MAX -- eleven seconds on
             * 2026-10-10, CRC error at 01:13:13, resend at 01:13:24 -- while
             * the terminal, which had already sent its ACK, waited for the
             * next packet and gave up: "disconnect status Failure" 3 s after
             * the resend, mid-INSTALL. Ending the frame after one timeout
             * makes the caller resend at once, which is what the terminal is
             * waiting for. Between packets (SEARCH_FOR_START) nothing changes.
             */
            if (l2_state != L2_STATE_SEARCH_FOR_START) {
                printf("%s: Packet cut short (no byte for a second mid-packet), status=0x%02x.\n",
                       __func__, status);
                return status | PKT_ERROR_FRAMING;
            }

            /*
             * An ACK that has not started within PKT_ACK_TIMEOUT never will.
             *
             * patches/0005 caught an ACK that arrived with a byte missing.
             * The next INSTALL (2026-10-10 01:28:58) lost one whole: 87% into
             * the second display-prompt table the terminal's ACK never came,
             * the manager waited out PKT_TIMEOUT_MAX (12 s) before resending,
             * and the terminal had hung up by then, so the resend found no
             * one. A table packet takes about 2.1 s on the wire at 1200 baud
             * and the terminal ACKs at once -- the bench log shows a packet
             * every 2.7-3 s -- so six seconds with no byte at all while an
             * ACK is due is a lost ACK, and resending then still lands inside
             * the terminal's patience. Only while an ACK is awaited: a
             * terminal's own messages keep the long wait.
             */
            if (proto->waiting_for_ack && timeout > PKT_ACK_TIMEOUT) {
                printf("%s: No ACK after %d s, resending.\n", __func__, PKT_ACK_TIMEOUT);
                return PKT_ERROR_TIMEOUT;
            }

            if (timeout > PKT_TIMEOUT_MAX) {
                printf("%s: Timeout waiting for packet error.\n", __func__);
                status = PKT_ERROR_TIMEOUT;
                return status;
            }
        }

        if (bytes_read == MODEM_RSP_READ_ERROR) {
            fprintf(stderr, "%s: Error reading from modem, bailing.\n", __func__);
            proto_disconnect(proto);
            return PKT_ERROR_FAILURE;
        }

        timeout = 0;

        /*
         * "NO CARRIER" arriving in the data stream means the call is gone.
         *
         * A real modem reports a dropped call by lowering DCD, which is what
         * monitor_carrier polls. A pty has no DCD, so -w turns that off, and
         * then nothing noticed a hangup at all: the modem's NO CARRIER was
         * parsed as line noise and the manager sat in a dead session until
         * its own timeouts gave up. That bit every Millennium INSTALL, whose
         * answer-detect check places a short call to the manager and hangs
         * up: the download call arriving ~20 s later landed inside that dead
         * session, got "Expected NACK", and failed; the retry worked.
         *
         * Matched only between packets (SEARCH_FOR_START), so the bytes of
         * a framed packet can never trip it.
         */
        if (l2_state == L2_STATE_SEARCH_FOR_START) {
            static const char no_carrier[] = "NO CARRIER";
            static size_t nc_matched = 0;

            if (databyte == (uint8_t)no_carrier[nc_matched]) {
                if (++nc_matched == sizeof(no_carrier) - 1) {
                    nc_matched = 0;
                    printf("%s: NO CARRIER from modem, call ended.\n", __func__);
                    proto_disconnect(proto);
                    return PKT_ERROR_NO_CARRIER;
                }
            } else {
                nc_matched = (databyte == (uint8_t)no_carrier[0]) ? 1 : 0;
            }
        }

        switch (l2_state) {
            case L2_STATE_SEARCH_FOR_START:
                if (databyte == START_BYTE) {
                    l2_state         = L2_STATE_GET_FLAGS;
                    pkt->payload_len = 0;
                    pkt->hdr.start   = databyte;
                }
                break;
            case L2_STATE_GET_FLAGS:
                l2_state       = L2_STATE_GET_LENGTH;
                pkt->hdr.flags = databyte;
                break;
            case L2_STATE_GET_LENGTH:
                pkt->hdr.pktlen = databyte;

                if (pkt->hdr.pktlen > 5) {
                    l2_state = L2_STATE_ACCUMULATE_DATA;
                } else {
                    l2_state = L2_STATE_GET_CRC0;
                }
                break;
            case L2_STATE_ACCUMULATE_DATA:
                pkt->payload[pkt->payload_len] = databyte;
                pkt->payload_len++;

                if (pkt->payload_len == pkt->hdr.pktlen - 5) {
                    l2_state = L2_STATE_GET_CRC0;
                }
                break;
            case L2_STATE_GET_CRC0:
                l2_state         = L2_STATE_GET_CRC1;
                pkt->trailer.crc = databyte;
                break;
            case L2_STATE_GET_CRC1:
                l2_state            = L2_STATE_SEARCH_FOR_STOP;

                pkt->trailer.crc   |= (uint16_t)(databyte << 8);
                pkt->trailer.crc    = LE16(pkt->trailer.crc);
                pkt->calculated_crc = crc16(0, &pkt->hdr.start, 3);
                pkt->calculated_crc = crc16(pkt->calculated_crc, pkt->payload, (size_t)pkt->payload_len);
                pkt->calculated_crc = LE16(pkt->calculated_crc);

                if (pkt->trailer.crc != pkt->calculated_crc) {
                    printf("%s: CRC Error!\n", __func__);
                    status |= PKT_ERROR_CRC;
                }
                break;
            case L2_STATE_SEARCH_FOR_STOP:
                if (databyte == STOP_BYTE) {
                    l2_state = L2_STATE_SEARCH_FOR_START;
                } else {
                    printf("%s: Framing Error!\n", __func__);
                    status |= PKT_ERROR_FRAMING;
                }
                pkt->trailer.end = databyte;
                pkt_received     = 1;
                break;
        }
    }

    /* Copy the packet trailer (CRC-16, STOP) immediately following the data */
    memcpy(&(pkt->payload[pkt->payload_len]), &pkt->trailer, sizeof(pkt->trailer));

    mm_add_pcap_rec(proto->pcapstream, RX, pkt, 0, 0);
    if (proto->send_udp) {
        mm_udp_send_pkt(RX, pkt);
    }

    if (pkt->hdr.flags & FLAG_RETRY) {
        if (proto->debuglevel > 0) print_mm_packet(RX, pkt);
        status |= PKT_ERROR_RETRY;
    }

    if (pkt->hdr.flags & FLAG_DISCONNECT) {
        if (proto->debuglevel > 0) print_mm_packet(RX, pkt);
        printf("%s: Received disconnect status %s from terminal.\n", __func__,
               pkt->hdr.flags & FLAG_STATUS ? "Failure" : "OK");
        proto->tx_seq = 0;

        printf("%s: Hanging up modem.\n", __func__);
        proto_disconnect(proto);
        status |= PKT_ERROR_DISCONNECT;
    }

    if (proto->debuglevel > 3) {
        printf("\nRaw Packet received: ");

        dump_hex(&pkt->hdr.start, (size_t)pkt->hdr.pktlen + 1);
    }

    return status;
}

/* Send manager packet to the terminal.
 *
 * If payload is NULL, an ACK packet will be sent.
 * If payload is not NULL, and the length is 0, a NULL packet will be sent.
 * If payload is not NULL, and length is > 0, then the terminal's phone number
 * will be prepended to the payload and sent.
 *
 * Returns PKT_SUCCESS on success, otherwise PKT_ERROR_ code flags.
 */
static pkt_status_t send_mm_packet(mm_proto_t* proto, uint8_t* payload, size_t len, uint8_t flags) {
    mm_packet_t pkt;
    pkt_status_t status = PKT_SUCCESS;
    int retries;

    for (retries = 0; retries < PKT_MAX_RETRIES; retries++) {
        /* Bail out if not connected. */
        if (!proto->connected) {
            return PKT_ERROR_DISCONNECT;
        }

        if (proto->debuglevel > 3) {
            if (payload != NULL) {
                printf("T<--M Sending packet: Terminal: %s, tx_seq=%d\n", proto->terminal_id, proto->tx_seq);
            }
            else {
                printf("T<--M Sending %s: rx_seq=%d\n", (flags & FLAG_ACK) ? "ACK" : "NACK", proto->rx_seq);
            }
        }

        /* Insert Tx packet delay when using a modem, in 10ms increments. */
        if (proto->use_modem) {
#ifdef _WIN32
            Sleep(proto->rx_packet_gap * 10);
#else  /* ifdef _WIN32 */
            struct timespec tim;
            tim.tv_sec = 0;
            tim.tv_nsec = proto->rx_packet_gap * 10000000L;
            nanosleep(&tim, NULL);
#endif /* _WIN32 */
        }

        memset(&pkt, 0, sizeof(pkt));
        pkt.hdr.start = START_BYTE;

        if (payload != NULL) {
            /* Flags for regular TX packet use tx_seq. */
            pkt.hdr.flags = (proto->tx_seq & FLAG_SEQUENCE);
            pkt.payload_len = (uint8_t)len + PKT_TABLE_ID_OFFSET; /* add room for the phone number. */

            for (int i = 0; i < PKT_TABLE_ID_OFFSET; i++) {
                pkt.payload[i] = (proto->terminal_id[i * 2] - '0') << 4;
                pkt.payload[i] |= (proto->terminal_id[i * 2 + 1] - '0');
            }

            if (len > 0) {
                memcpy(&pkt.payload[PKT_TABLE_ID_OFFSET], payload, len);
            }
        } else {
            pkt.payload_len = 0;

            /* If payload is NULL, send an ACK packet instead, using rx_seq. */
            pkt.hdr.flags = flags | (proto->rx_seq & FLAG_SEQUENCE);
        }

        if (flags & FLAG_RETRY) {
            pkt.hdr.flags |= FLAG_RETRY;
        }

        pkt.hdr.pktlen = pkt.payload_len + 5;
        pkt.trailer.crc = crc16(0, &pkt.hdr.start, 3);
        pkt.trailer.crc = crc16(pkt.trailer.crc, pkt.payload, (size_t)(pkt.payload_len));
        pkt.trailer.crc = LE16(pkt.trailer.crc);
        if (inject_comm_error == 1) {
            if (((proto->error_inject_type == ERROR_INJECT_CRC_DLOG_TX) && (pkt.payload_len != 0)) ||
                ((proto->error_inject_type == ERROR_INJECT_CRC_ACK_TX) && (pkt.payload_len == 0))) {
                printf("Injecting %s (Correct CRC=0x%02x).\n",
                    error_inject_type_to_str(proto->error_inject_type),
                    pkt.trailer.crc);
                inject_comm_error = 0;
                pkt.trailer.crc = ~pkt.trailer.crc;
            }
        }
        pkt.trailer.end = STOP_BYTE;
        pkt.calculated_crc = pkt.trailer.crc;

        /* Copy the CRC and STOP_BYTE to be adjacent to the filled portion of the payload */
        memcpy(&(pkt.payload[pkt.payload_len]), &pkt.trailer.crc, 3);

        mm_add_pcap_rec(proto->pcapstream, TX, &pkt, 0, 0);
        if (proto->send_udp) {
            mm_udp_send_pkt(TX, &pkt);
        }

        if (proto->debuglevel > 0) {
            print_mm_packet(TX, &pkt);
        }

        if (proto->debuglevel > 3) {
            printf("\nRaw Packet transmitted: ");
            dump_hex(&pkt.hdr.start, (size_t)pkt.hdr.pktlen + 1);
        }

        write_serial(proto->serial_context, &pkt, (size_t)pkt.hdr.pktlen + 1);
        drain_serial(proto->serial_context);

        /* Don't wait for ACK if sending an ACK. */
        if (payload == NULL) {
            break;
        }

        status = wait_for_mm_ack(proto);
        if (status == PKT_SUCCESS) {
            break;
        }

        printf("%s: Received NACK, retrying %d.\n", __func__, retries);
    }

    if (retries == PKT_MAX_RETRIES) {
        printf("%s: Error: Gave up after %d retries.\n", __func__, retries);
        status |= PKT_ERROR_FAILURE;
    }

    if (payload != NULL) {
        proto->tx_seq++;
    } else {
        proto->rx_seq++;
    }
    return status;
}

static pkt_status_t send_mm_ack(mm_proto_t *proto, uint8_t flags) {
    return send_mm_packet(proto, NULL, 0, flags);
}

static pkt_status_t wait_for_mm_ack(mm_proto_t *proto) {
    mm_packet_t pkt;
    pkt_status_t status = PKT_SUCCESS;

    if (!proto->connected) {
        return PKT_ERROR_DISCONNECT;
    }

    proto->waiting_for_ack = 1;
    status = receive_mm_packet(proto, &pkt);
    proto->waiting_for_ack = 0;
    if ((status == PKT_SUCCESS) || (status == PKT_ERROR_DISCONNECT)) {
        if (proto->debuglevel > 2) print_mm_packet(RX, &pkt);

        if (pkt.payload_len == 0) {
            if (pkt.hdr.flags & FLAG_ACK) {
                return PKT_SUCCESS;
            } else {
                /* ACK flag is not set: NACK. */
                return PKT_ERROR_NACK;
            }
        }
        return PKT_SUCCESS;
    }
    fprintf(stderr, "%s: Error, did not receive an ACK packet, status=0x%02x\n", __func__, status);
    return status;
}

