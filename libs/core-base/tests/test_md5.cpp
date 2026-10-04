#include <doctest/doctest.h>

#include <string>

#include "supple/base/md5.h"

using namespace supple;

TEST_CASE("MD5: RFC 1321's test suite, and incremental updates match one call") {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5Hex("1234567890123456789012345678901234567890123456789012345678901234567890123456"
                 "7890") == "57edf4a22be3c955ac49da2e2107b67a");
    std::string text;
    for (int i = 0; i < 1000; ++i) text += static_cast<char>('a' + i % 26);
    Md5 pieces;
    for (std::size_t at = 0; at < text.size(); at += 37)
        pieces.update(std::string_view(text).substr(at, 37));
    const auto a = pieces.finish();
    Md5 whole;
    whole.update(text);
    CHECK(a == whole.finish());
}
