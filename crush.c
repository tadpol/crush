#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

enum crush_commands_e {
    crush_cmd_req_write = 1,
    crush_cmd_rpl_write = 9,
    crush_cmd_req_read = 2,
    crush_cmd_rpl_read = 10,
    crush_cmd_req_jump = 3,
    crush_cmd_rpl_jump = 11,
    crush_cmd_msg_indicate = 4,
};
enum crush_parser_state_e {
    crush_ps_looking_for_b,
    crush_ps_get_cmd,
    crush_ps_get_length,
    crush_ps_get_rest,
};
typedef enum crush_parser_state_e crush_parser_state_t;

struct crush_state_s {
    crush_parser_state_t state;
    uint8_t buffer[40];
    uint8_t *wkbk;
    uint8_t length;
} crush = {
    .state = crush_ps_looking_for_b,
};

uint16_t crush_fletcher16(void* data, size_t length);

void crush_send(uint8_t cmd, void* addr, void* data, size_t dlen)
{
    uint8_t buf[40];
    uint8_t *p = buf;
    ptrdiff_t a = (ptrdiff_t)addr, m=0;
    int alen;
    uint16_t cksm;

    for(alen=1; alen > 0xf; alen++) {

        if((a & m) == a) break;
    }

    *p++ = (cmd << 4) | alen;
    p++; // length will get filled later

    // addr

    memcpy(p, data, dlen);

    buf[1] = (p - buf) + 2;

    cksm = crush_fletcher16(buf, buf[1] - 2);
    *p++ = cksm >> 8;
    *p = cksm;

    put(buf);
}

int crush_validate_checksum(void)
{
    uint16_t lcksm, ccksm;
    uint8_t len = crush.buffer[1];

    lcksm  = crush.buffer[len-2] << 8;
    lcksm |= crush.buffer[len-1];

    ccksm = crush_fletcher16(crush.buffer, len - 2);

    return lcksm == ccksm;
}

void* crush_get_address(void)
{
    ptrdiff_t addr=0;
    // TODO verify that it fits.
    int alen = crush.buffer[0] & 0xf;
    uint8_t *p = &crush.buffer[2];
    for(; alen > 0; alen--,p++) {
        addr <<= 8;
        addr |= *p;
    }
    return (void*)addr;
}

void crush_do_cmd(void)
{
    uint8_t cmd = crush.buffer[0] >> 4;
    uint8_t alen = crush.buffer[0] & 0xf;
    uint8_t *addr = crush_get_address();
    uint8_t *data = crush.buffer + 2 + alen;
    size_t dlen = crush.buffer[1] - 4 - alen;

    if(cmd == crush_cmd_req_write) {
        memcpy(addr, data, dlen);
        crush_send(crush_cmd_rpl_write, addr, NULL, 0);

    } else if(cmd == crush_cmd_req_read) {
        unsigned chunk;
        unsigned readlen=0;
        /* Read the requested length */
        for(; dlen > 0; dlen--, data++) {
            readlen <<= 8;
            readlen |= *data;
        }

        /* now read */
        for(; readlen > 0; ) {
            chunk = (readlen>34)?34:readlen;

            crush_send(crush_cmd_rpl_read, addr, (uint8_t*)addr, chunk);

            readlen -= chunk;
            addr += chunk;
        }

    } else if(cmd == crush_cmd_req_jump) {
        ((void(*)(void))addr)();
        crush_send(crush_cmd_rpl_jump, addr, NULL, 0);
    }
}

int crush_inchar(int c)
{
    if(crush.state == crush_ps_looking_for_b) {
        if(c == '\b') {
            crush.state = crush_ps_get_cmd;
            crush.wkbk = crush.buffer;
            c=-1;
        }

    }else if(crush.state == crush_ps_get_cmd) {
        *crush.wkbk++ = c;
        crush.state = crush_ps_get_length;
        c=-1;

    }else if(crush.state == crush_ps_get_cmd) {
        *crush.wkbk++ = c;
        crush.length = c - 2; /* subtract the two we already have */
        crush.state = crush_ps_get_rest;
        c=-1;

    }else if(crush.state == crush_ps_get_rest) {
        *crush.wkbk++ = c;
        crush.length--;
        if(crush.length == 0 && crush_validate_checksum()) {
            crush_do_cmd();
            crush.state = crush_ps_looking_for_b;
        }
        c=-1;
    } 

    return c;
}


/* vim: set ai cin et sw=4 ts=4 : */
