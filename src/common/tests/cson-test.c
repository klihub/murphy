/*
 * Copyright (c) 2016, Krisztian Litkey
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Author nor the names of other contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <murphy/common/macros.h>
#include <murphy/common/debug.h>
#include <murphy/common/mm.h>
#include <murphy/common/cson.h>

int main(int argc, char *argv[])
{
    mrp_cson_t *s, *i, *b, *d, *o, *v, *a;
    void *test;
    int8_t i8;
    int16_t i16;
#if 0
    int32_t i32;
    int64_t i64;
#endif
    mrp_cson_t *co;
    char **str;
    char *strings[] = {
        "string", "another one", "a test string",
        "foo", "foobar", "the quick brown frox jumps over the lazy dog",
        NULL
    };
    int arg;

    if (argc > 1 && (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--debug"))) {
        mrp_debug_enable(true);
        if (argc > 2) {
            for (arg = 2; arg < argc; arg++)
                mrp_debug_set(argv[arg]);
        }
        else
            mrp_debug_set("*");
    }

    mrp_cson_set_default_mode(MRP_CSON_TYPE_COMPACT);

    test = mrp_allocz(8192);
    printf("test pointer: %p (0x%llx)\n", test, (unsigned long long)test);

    printf("__MRP_PTRBITS: %d\n", __MRP_PTRBITS);
    printf("sizeof(int): %zu, sizeof(long): %zu, sizeof(ptrdiff_t): %zu\n",
           sizeof(int), sizeof(long), sizeof(ptrdiff_t));

    printf("compact: 0x%llx\n", MRP_CSON_COMPACT_BIT);
    printf(" int8<<: 0x%llx\n",
           ((unsigned long long)MRP_CSON_TYPE_INT8) << MRP_CSON_TYPE_SHIFT);
    printf(" int64<: 0x%llx\n",
           ((unsigned long long)MRP_CSON_TYPE_UINT64) << MRP_CSON_TYPE_SHIFT);

    printf("    char: %d - %d\n"  , SCHAR_MIN        , SCHAR_MAX         );
    printf("     int: %d - %d\n"  , INT_MIN          , INT_MAX           );
    printf("  int8_t: %d - %d\n"  , MRP_CSON_MININT8 , MRP_CSON_MAXINT8  );
    printf(" int16_t: %d - %d\n"  , MRP_CSON_MININT16, MRP_CSON_MAXINT16 );
    printf(" int32_t: %d - %d\n"  , MRP_CSON_MININT32, MRP_CSON_MAXINT32 );
    printf(" int64_t: %ld - %ld\n", MRP_CSON_MININT64, MRP_CSON_MAXINT64 );
    printf(" uint8_t: %u - %u\n"  , 0                , MRP_CSON_MAXUINT8 );
    printf("uint16_t: %u - %u\n"  , 0                , MRP_CSON_MAXUINT16);
    printf("uint32_t: %u - %u\n"  , 0                , MRP_CSON_MAXUINT32);
    printf("uint64_t: %lu - %lu\n", 0L               , MRP_CSON_MAXUINT64);

    for (i8 = MRP_CSON_MININT8; i8 < MRP_CSON_MAXINT8; i8++) {
        co = mrp_cson_create(MRP_CSON_TYPE_INT8, i8);
        v  = (mrp_cson_t *)mrp_cson_compact_value(co);

        printf("int8_t %d: 0x%x (-0x%x), co: %p, v: %p\n", i8, i8, -i8, co, v);

        if ((ptrdiff_t)v != i8) {
            printf("int8_t broken...\n");
            exit(1);
        }
    }

    for (i16 = MRP_CSON_MININT16; i16 < MRP_CSON_MAXINT16; i16++) {
        co = mrp_cson_create(MRP_CSON_TYPE_INT16, i16);
        v  = (mrp_cson_t *)mrp_cson_compact_value(co);

        if (!(i16 % 1024) || (ptrdiff_t)v != i16)
        printf("int16_t %d: 0x%x, co: %p, v: %p\n", i16, i16, co, v);

        if ((ptrdiff_t)v != i16) {
            printf("int16_t broken...\n");
            exit(1);
        }
    }

#if 0
    for (i32 = MRP_CSON_MININT32; i32 < MRP_CSON_MAXINT32; i32++) {
        co = mrp_cson_create(MRP_CSON_TYPE_INT32, i32);
        v  = (mrp_cson_t *)mrp_cson_compact_value(co);

        if (!(i32 % (4 * 1024 * 1024)) || (ptrdiff_t)v != i32)
            printf("int32_t %d: 0x%x, co: %p, v: %p\n", i32, i32, co, v);

        if ((ptrdiff_t)v != i32) {
            printf("int32_t broken...\n");
            exit(1);
        }
    }

    for (i64 = MRP_CSON_MININT64; i64 < MRP_CSON_MAXINT64; i64++) {
        co = mrp_cson_create(MRP_CSON_TYPE_INT64, i64);
        v  = (mrp_cson_t *)mrp_cson_compact_value(co);

        if (!(i64 % (64 * 1024 * 1024)) || (ptrdiff_t)v != i64)
            printf("int64_t %lld: 0x%llx, co: %p, v: 0x%llx\n",
                   (unsigned long long)i64, (unsigned long long)i64, co, v);

        if ((ptrdiff_t)v != i64) {
            printf("int64_t broken...\n");
            exit(1);
        }
    }
#endif

    for (str = strings; *str; str++) {
        co = mrp_cson_create(MRP_CSON_TYPE_STRING, *str);

        if (mrp_cson_get_type(co) != MRP_CSON_TYPE_STRING) {
            printf("string compact type mismatch\n");
            exit(1);
        }

        v = (mrp_cson_t *)mrp_cson_compact_value(co);

        if (strcmp(*str, (char *)v)) {
            printf("compact string value mismatch for '%s'\n", *str);
        }
        else
            printf("compact string value: '%s'\n", (char *)v);
    }


    s = mrp_cson_create(MRP_CSON_STRING("a string"));
    i = mrp_cson_create(MRP_CSON_INTEGER(1));
    b = mrp_cson_create(MRP_CSON_BOOLEAN(true));
    d = mrp_cson_create(MRP_CSON_DOUBLE(3.141));
    o = mrp_cson_create(MRP_CSON_TYPE_OBJECT);
    a = mrp_cson_create(MRP_CSON_TYPE_ARRAY);

    fprintf(stdout, "%p: %CSONp\n", s, s);
    fprintf(stdout, "%p: %CSONp\n", i, i);
    fprintf(stdout, "%p: %CSONp\n", b, b);
    fprintf(stdout, "%p: %CSONp\n", d, d);

    fprintf(stdout, "%p: %#CSONp\n", s, s);
    fprintf(stdout, "%p: %#CSONp\n", i, i);
    fprintf(stdout, "%p: %#CSONp\n", b, b);
    fprintf(stdout, "%p: %#CSONp\n", d, d);

    if (mrp_cson_set(o, "string" , s) < 0)
        printf("setting 'string' member failed\n");
    if (mrp_cson_set(o, "integer", i) < 0)
        printf("setting 'integer' member failed\n");
    if (mrp_cson_set(o, "boolean", b) < 0)
        printf("setting 'boolean' member failed\n");
    if (mrp_cson_set(o, "double" , d) < 0)
        printf("setting 'double' member failed\n");
    if (mrp_cson_set(o, "array" , a) < 0)
        printf("setting 'array' member failed\n");

    if (mrp_cson_get(o, "string") != s)
        printf("getting 'string' member failed\n");
    if (mrp_cson_get(o, "integer") != i)
        printf("getting 'integer' member failed\n");
    if (mrp_cson_get(o, "boolean") != b)
        printf("getting 'boolean' member failed\n");
    if (mrp_cson_get(o, "double") != d)
        printf("getting 'double' member failed\n");
    if (mrp_cson_get(o, "array") != a)
        printf("getting 'array' member failed\n");

#if 0
    if (mrp_cson_set_string(o, "string", "another string") < 0)
        printf("setting 'string' member failed\n");

    if (mrp_cson_set_boolean(o, "true-value", TRUE) < 0)
        printf("setting 'true-value' member failed\n");

    if (mrp_cson_set_boolean(o, "false-value", FALSE) < 0)
        printf("setting 'false-value' member failed\n");

    if (mrp_cson_set_integer(o, "int", 1973) < 0)
        printf("setting 'int' member failed\n");

    if (mrp_cson_get_boolean(o, "true-value") != TRUE)
        printf("getting 'true-value' member mismatch\n");

    if (mrp_cson_get_boolean(o, "false-value") != FALSE)
        printf("getting 'false-value' member mismatch\n");

    if (mrp_cson_get_integer(o, "int") != 1973)
        printf("getting 'int' member mismatch\n");
#endif

    fprintf(stdout, "%p: pretty: %#CSONp\n", o, o);
    fprintf(stdout, "%p: normal: %CSONp\n", o, o);

    mrp_cson_unref(s);
    mrp_cson_unref(i);
    mrp_cson_unref(b);
    mrp_cson_unref(d);

    return 0;
}
