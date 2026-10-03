#include <cassert>
#include <cstring>
#include <iostream>

#include "common/uuid.h"
#include "pool/thread_pool.h"
#include "reactor/buffer.h"
#include "reactor/http_parser.h"

using namespace transcode;

static void testUuid() {
    std::string id = generateUuid();
    assert(id.size() == 36);
    assert(id[8] == '-');
    std::cout << "[PASS] uuid: " << id << "\n";
}

static void testBuffer() {
    Buffer buf;
    buf.append("GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n", 39);
    assert(buf.findCRLF() != nullptr);
    buf.retrieveAll();
    assert(buf.readableBytes() == 0);
    std::cout << "[PASS] buffer findCRLF/retrieveAll\n";
}

static void testThreadPool() {
    ThreadPool pool(2, 2);
    pool.start();
    assert(pool.tryEnqueue([]() {}));
    pool.stop();
    assert(!pool.tryEnqueue([]() {}));
    std::cout << "[PASS] thread pool bounded queue\n";
}

static void testHttpParser() {
    HttpParser parser;
    Buffer buf;
    const char* req = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    buf.append(req, std::strlen(req));
    assert(parser.parse(&buf) == ParseResult::kComplete);
    assert(parser.request().method == "GET");
    assert(parser.request().path == "/healthz");
    std::cout << "[PASS] http parser GET request\n";
}

int main() {
    testUuid();
    testBuffer();
    testThreadPool();
    testHttpParser();
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
