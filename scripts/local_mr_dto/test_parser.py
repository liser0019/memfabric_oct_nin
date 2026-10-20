#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Compile the real local-MR parser/writer with CPU-only dependency substitutes.

The production BinaryStream header, record type, parser and writer are read from
source at test time. No transport, device loader or full HCOMM ABI is exercised.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TRANSPORT = ROOT / "src/legacy/ascend950/unified_platform/resource/transport"


def definition(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREAMBLE = r'''
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
namespace Hccl {
using InternalException = std::runtime_error;
template<class... Args> void TestLog(Args &&...) {}
template<class... Args> std::string StringFormat(const char *fmt, Args... args)
{
    const int length = std::snprintf(nullptr, 0, fmt, args...);
    std::vector<char> text(length + 1);
    std::snprintf(text.data(), text.size(), fmt, args...);
    return std::string(text.data());
}
}
#define MACRO_THROW(type, message) throw type(message)
#define HCCL_INFO(...) Hccl::TestLog(__VA_ARGS__)
#define HCCL_WARNING(...) Hccl::TestLog(__VA_ARGS__)
#define HCCL_ERROR(...) Hccl::TestLog(__VA_ARGS__)
#include "binary_stream.h"
namespace Hccl {
'''

MAIN = r'''
}
using namespace Hccl;
void Require(bool ok, const std::string &message)
{
    if (!ok) throw std::runtime_error(message);
}
int main()
{
    try {
        UbMemTransport writer;
        const std::vector<UbTransportLiteImpl::LocUbBufLite> expected = {
            {0x120000017000ULL, 0x1000, 7, 0x13579bdf},
            {0x340000000000ULL, 0x400000, 13, 0x2468ace0},
            {0x124000800000ULL, 0x200000, 23, 0xabcd1234}
        };
        unsigned passed = 0;
        for (u32 width : {24U, 28U}) {
            for (u32 count : {1U, 2U, 3U}) {
                std::vector<char> data;
                for (u32 i = 0; i < count; ++i) {
                    const auto &item = expected[i];
                    auto record = writer.GetSingleRmtBufferUniqueId(
                        item.addr, item.size, item.tokenId, item.tokenValue, 0x55667700U + i);
                    Require(record.size() == 28U, "actual writer must emit 28 bytes");
                    if (width == 24U) record.resize(24U); // legacy four-field layout
                    data.insert(data.end(), record.begin(), record.end());
                }
                UbTransportLiteImpl parser;
                parser.bufferNum = count;
                parser.ParseLocBufferMap(data);
                Require(parser.locBufferMap.size() == count, "wrong MR count");
                for (u32 i = 0; i < count; ++i) {
                    const auto &item = expected[i];
                    auto found = parser.locBufferMap.find(item.addr);
                    std::string context = std::to_string(width) + "-byte record " + std::to_string(i);
                    Require(found != parser.locBufferMap.end(), "wrong address: " + context);
                    const auto &actual = found->second;
                    Require(actual.size == item.size && actual.tokenId == item.tokenId &&
                            actual.tokenValue == item.tokenValue, "wrong MR fields: " + context);
                }
                ++passed;
            }
        }
        for (size_t bytes : {0U, 23U, 25U, 27U, 29U, 47U, 55U, 57U, 60U, 64U}) {
            UbTransportLiteImpl parser;
            parser.bufferNum = 2;
            std::vector<char> data(bytes);
            bool rejected = false;
            try { parser.ParseLocBufferMap(data); }
            catch (const InternalException &) { rejected = true; }
            Require(rejected && parser.locBufferMap.empty(), "accepted invalid size " + std::to_string(bytes));
            ++passed;
        }
        UbTransportLiteImpl empty;
        std::vector<char> noData;
        empty.ParseLocBufferMap(noData);
        Require(empty.locBufferMap.empty(), "zero-count behavior changed");
        std::cout << "PASS: " << passed + 1 << " parser cases (real writer, record and BinaryStream)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
'''


def make_source(parser_path):
    record = definition((TRANSPORT / "aicpu/ub_transport_lite_impl.h").read_text(), "struct LocUbBufLite")
    parser = definition(parser_path.read_text(), "void UbTransportLiteImpl::ParseLocBufferMap")
    writer = definition((TRANSPORT / "ub_mem_transport.cc").read_text(),
                        "std::vector<char> UbMemTransport::GetSingleRmtBufferUniqueId")
    classes = f"""
class UbTransportLiteImpl {{
public:
    {record};
    u32 bufferNum = 0;
    std::map<uintptr_t, LocUbBufLite> locBufferMap;
    void ParseLocBufferMap(std::vector<char> &data);
}};
class UbMemTransport {{
public:
    std::vector<char> GetSingleRmtBufferUniqueId(u64, u64, u32, u32, u32) const;
}};
"""
    return PREAMBLE + classes + parser + "\n" + writer + MAIN


def main():
    options = argparse.ArgumentParser(description=__doc__)
    options.add_argument("--source", type=Path, default=TRANSPORT / "aicpu/ub_transport_lite_impl.cc")
    options.add_argument("--sanitize", action="store_true")
    args = options.parse_args()
    with tempfile.TemporaryDirectory(prefix="hcomm-local-mr-") as path:
        work = Path(path)
        (work / "hccl").mkdir()
        for name in ("hccl/base.h", "exception_util.h", "log.h"):
            (work / name).write_text("#pragma once\n")
        shutil.copyfile(ROOT / "src/legacy/ascend950/common/binary_stream.h", work / "binary_stream.h")
        (work / "test.cpp").write_text(make_source(args.source))
        command = shlex.split(os.environ.get("CXX", "c++"))
        command += ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-I", str(work)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command + [str(work / "test.cpp"), "-o", str(work / "test")], check=True)
        subprocess.run([str(work / "test")], check=True)


if __name__ == "__main__":
    main()
