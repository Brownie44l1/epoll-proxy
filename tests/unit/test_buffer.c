/* Unit tests for buffer module */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "buffer.h"

void test_buffer_init() {
    buffer_t buf;
    buffer_init(&buf);
    
    assert(buf.len == 0);
    assert(buf.pos == 0);
    assert(buffer_is_empty(&buf));
    assert(!buffer_is_full(&buf));
    
    printf("✓ test_buffer_init passed\n");
}

void test_buffer_append() {
    buffer_t buf;
    buffer_init(&buf);
    
    const char *data = "Hello, World!";
    size_t len = strlen(data);
    
    size_t written = buffer_append(&buf, data, len);
    assert(written == len);
    assert(buf.len == len);
    assert(memcmp(buf.data, data, len) == 0);
    
    printf("✓ test_buffer_append passed\n");
}

void test_buffer_clear() {
    buffer_t buf;
    buffer_init(&buf);
    
    buffer_append(&buf, "test", 4);
    buffer_clear(&buf);
    
    assert(buf.len == 0);
    assert(buf.pos == 0);
    assert(buffer_is_empty(&buf));
    
    printf("✓ test_buffer_clear passed\n");
}

int main() {
    printf("Running buffer tests...\n");
    
    test_buffer_init();
    test_buffer_append();
    test_buffer_clear();
    
    printf("\n✅ All buffer tests passed!\n");
    return 0;
}
