#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>


enum crush_parse_state_e {
    crush_state_waiting_for_cmd = 0,
    crush_state_type,
    crush_state_length_high,
    crush_state_length_low,
    crush_state_addr,
    crush_state_wbk_high,
    crush_state_wbk_low,
    crush_state_checksum,

};
typedef enum crush_parse_state_e crush_parse_state_t;

enum crush_status_e {
    crush_status_parse_error = 1,
    crush_status_checksum_error = 1<<1,
};

uint16_t crush_status=0;
uint8_t crush_wbk[80];

static struct crush_state_s {
    crush_parse_state_t state;
    uint8_t cmd;
    uint8_t type;
    uint8_t checksum;
    uint16_t length;
    uint32_t addr;
    uint8_t *wbk;
} crush = {
    .state = crush_state_waiting_for_cmd,
};

void crush_do_cmd(uint8_t cmd, uint64_t addr, uint8_t *data, int length);
void crush_send(uint8_t cmd, uint8_t type, uint32_t addr, uint8_t *data, int dlen);
void crush_indicate(void *addr, void* data, int length);

static int crush_fromnibble(int c)
{
    if( c >= '0' && c <= '9' ) c -= '0';
    else if( c >= 'a' && c <= 'f') c = (c - 'a') + 10;
    else if( c >= 'A' && c <= 'F') c = (c - 'A') + 10;
    else c = 0;
    return c;
}

void crush_parse_char(int c)
{
    if(crush.state == crush_state_waiting_for_cmd) {
        crush.type = 0;
        crush.checksum = 0;
        crush.length = 0;
        crush.addr = 0;
        crush.state = crush_state_type;
        if(c == 'W' || c == 'R' || c == 'J') {
            crush.cmd = c;
            crush.checksum += c;
        } else {
            crush.state = crush_state_waiting_for_cmd;
            /* or not, just keep eating then. */
        }
    } else if(crush.state == crush_state_type) {
        if(c >= '1' && c <= '3') {
            crush.type = c - '0';
            crush.checksum += c;
            crush.state = crush_state_length_high;
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_length_high) {
        if(isxdigit(c)) {
            crush.length = crush_fromnibble(c) << 4;
            crush.checksum += c;
            crush.state = crush_state_length_low;
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_length_low) {
        if(isxdigit(c)) {
            crush.length |= crush_fromnibble(c);
            crush.checksum += c;
            crush.state = crush_state_addr;
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_addr) {
        if(isxdigit(c)) {
            if(crush.type > 0) {
                crush.addr <<= 4;
                crush.addr |= crush_fromnibble(c);
                crush.checksum += c;
                crush.length --;
                crush.type --;
            } else {
                crush.state = crush_state_wbk_high;
                crush.wbk = crush_wbk;
            }
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_wbk_high) {
        if(isxdigit(c)) {
            *(crush.wbk) = crush_fromnibble(c) << 4;
            crush.checksum += c;
            crush.length --;
            crush.state = crush_state_wbk_low;
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_wbk_low) {
        if(isxdigit(c)) {
            *(crush.wbk) |= crush_fromnibble(c);
            crush.checksum += c;
            crush.length --;
            crush.wbk ++;

            if(crush.length == 0) {
                crush.state = crush_state_checksum;
            } else {
                crush.state = crush_state_wbk_high;
            }
        } else {
            crush.state = crush_state_waiting_for_cmd;
            crush_status |= crush_status_parse_error;
        }
    } else if(crush.state == crush_state_checksum) {
        /* The sum of all the parts and the checksum should result in 0 */
        crush.state = crush_state_waiting_for_cmd;
        if(crush.checksum == 0) {
            crush_do_cmd(crush.cmd, crush.addr, crush_wbk, (crush.wbk - crush_wbk) -1);
        } else {
            crush_status |= crush_status_checksum_error;
        }
    }
}

void crush_do_cmd(uint8_t cmd, uint64_t addr, uint8_t *data, int length)
{
    if(cmd == 'W') {
        memcpy((void*)addr, data, length);
        crush_send('w', 3, addr, NULL, 0);
    } else if(cmd == 'R') {
        unsigned chunk;
        unsigned dlen=0;
        /* Decode Request length from data */
        for(; length>0; length--, data++) {
            dlen <<=4;
            dlen = crush_fromnibble(*data);
        }

        /* Now read data in chunks. */
        for(; dlen > 0; ) {
            chunk = (dlen>34)?34:dlen; // 34 bytes with type 3, gives 80 char lines.

            crush_send('r', 3, addr, (uint8_t*)addr, chunk);

            dlen -= chunk;
            addr += chunk;
        }
    } else if(cmd == 'J') {
        uint16_t retval;
        retval = ((int(*)())addr)();
        crush_send('j', 3, addr, (uint8_t*)&retval, sizeof(uint16_t));
    }
}

void crush_send(uint8_t cmd, uint8_t type, uint32_t addr, uint8_t *data, int dlen)
{
    static const char nibbles[16] = "0123456789ABCDEF";
    uint8_t buf[80];
    uint8_t *p = buf;
    uint8_t checksum = 0;
    int length;

    *p = cmd, checksum += *p++;
    *p = type, checksum += *p++;

    length = (type + 1) * 2; /* length of the address */
    length += dlen * 2; /* Length of the data */
    length += 2; /* The checksum */

    *p = nibbles[(length >> 4) & 0xf], checksum += *p++;
    *p = nibbles[length & 0xf], checksum += *p++;

    switch(type) {
        case 3:
            *p = nibbles[(addr >> 28) & 0xf], checksum += *p++;
            *p = nibbles[(addr >> 24) & 0xf], checksum += *p++;
        case 2:
            *p = nibbles[(addr >> 20) & 0xf], checksum += *p++;
            *p = nibbles[(addr >> 16) & 0xf], checksum += *p++;
        case 1:
            *p = nibbles[(addr >> 12) & 0xf], checksum += *p++;
            *p = nibbles[(addr >> 8) & 0xf], checksum += *p++;
            *p = nibbles[(addr >> 4) & 0xf], checksum += *p++;
            *p = nibbles[(addr >> 0) & 0xf], checksum += *p++;
            break;
    }

    for(; dlen > 0; dlen--, data++) {
        *p = nibbles[(*data >> 4) & 0xf], checksum += *p++;
        *p = nibbles[*data & 0xf], checksum += *p++;
    }
    *p = nibbles[(checksum >> 4) & 0xf], checksum += *p++;
    *p = nibbles[checksum & 0xf], checksum += *p++;

    *p++ = '\n';
    *p = '\0';

    /* low level send */
    puts((const char*)buf);
}

void crush_indicate(void *addr, void* data, int length)
{
    crush_send('I', 3, (uint32_t)addr, data, length);
}

/* vim: set ai cin et sw=4 ts=4 : */