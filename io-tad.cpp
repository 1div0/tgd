/*
 * Copyright (C) 2018, 2019 Computer Graphics Group, University of Siegen
 * Written by Martin Lambers <martin.lambers@uni-siegen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <cstdio>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "io-tad.hpp"


namespace TAD {

FormatImportExportTAD::FormatImportExportTAD() :
    _f(nullptr),
    _arrayCount(-2)
{
}

FormatImportExportTAD::~FormatImportExportTAD()
{
    if (_f)
        fclose(_f);
}

Error FormatImportExportTAD::openForReading(const std::string& fileName, const TagList&)
{
    _f = fopen(fileName.c_str(), "rb");
    return _f ? ErrorNone : ErrorSysErrno;
}

Error FormatImportExportTAD::openForWriting(const std::string& fileName, bool append, const TagList&)
{
    _f = fopen(fileName.c_str(), append ? "ab" : "wb");
    return _f ? ErrorNone : ErrorSysErrno;
}

Error FormatImportExportTAD::close()
{
    if (_f) {
        if (fclose(_f) != 0) {
            _f = nullptr;
            return ErrorSysErrno;
        }
        _f = nullptr;
    }
    return ErrorNone;
}

static bool writeTadTagList(FILE* f, const TagList& tl)
{
    std::vector<char> data(sizeof(uint64_t), 0);
    for (auto it = tl.cbegin(); it != tl.cend(); it++) {
        data.insert(data.end(), it->first.c_str(), it->first.c_str() + it->first.length() + 1);
        data.insert(data.end(), it->second.c_str(), it->second.c_str() + it->second.length() + 1);
    }
    uint64_t n = data.size() - sizeof(uint64_t);
    std::memcpy(data.data(), &n, sizeof(uint64_t));
    return std::fwrite(data.data(), data.size(), 1, f) == 1;
}

static bool writeTad(FILE* f, const ArrayContainer& array)
{
    std::vector<uint8_t> start(5 + 2 * sizeof(uint64_t) + array.dimensionCount() * sizeof(uint64_t));
    start[0] = 'T';
    start[1] = 'A';
    start[2] = 'D';
    start[3] = 0;
    start[4] = array.componentType();
    uint64_t v;
    v = array.componentCount();
    std::memcpy(start.data() + 5, &v, sizeof(uint64_t));
    v = array.dimensionCount();
    std::memcpy(start.data() + 5 + sizeof(uint64_t), &v, sizeof(uint64_t));
    for (size_t d = 0; d < array.dimensionCount(); d++) {
        v = array.dimension(d);
        std::memcpy(start.data() + 5 + 2 * sizeof(uint64_t) + d * sizeof(uint64_t), &v, sizeof(uint64_t));
    }
    if (std::fwrite(start.data(), start.size(), 1, f) != 1
            || !writeTadTagList(f, array.globalTagList())) {
        return false;
    }
    for (size_t c = 0; c < array.componentCount(); c++) {
        if (!writeTadTagList(f, array.componentTagList(c)))
            return false;
    }
    for (size_t d = 0; d < array.dimensionCount(); d++) {
        if (!writeTadTagList(f, array.dimensionTagList(d)))
            return false;
    }
    if (std::fwrite(array.data(), array.dataSize(), 1, f) != 1) {
        return false;
    }
    return true;
}

static bool readString(const char* data, std::string& s, size_t& len)
{
    size_t i = 0;
    for (;;) {
        const char& c = data[i++];
        if (c == 0)
            break;
        if (c < 32 || c == 127)
            return false;
    }
    len = i;
    s = std::string(data);
    return true;
}

static Error readTadTagList(FILE* f, TagList& tl)
{
    uint64_t n;
    if (std::fread(&n, sizeof(uint64_t), 1, f) != 1)
        return ErrorSysErrno;
    if (n > std::numeric_limits<size_t>::max())
        return ErrorInvalidData;
    std::vector<char> data(n);
    if (std::fread(data.data(), data.size(), 1, f) != 1)
        return ErrorSysErrno;
    for (size_t i = 0; i < data.size(); ) {
        std::string key, value;
        size_t keyLen, valueLen;
        if (!readString(data.data() + i, key, keyLen)
                || i + keyLen + 1 >= data.size()
                || !readString(data.data() + i + keyLen + 1, value, valueLen)) {
            return ErrorInvalidData;
        }
        i += keyLen + 1 + valueLen + 1;
        tl.set(key, value);
    }
    return ErrorNone;
}

static Error readTadHeader(FILE* f, ArrayContainer& array)
{
    uint8_t start[5 + 2 * sizeof(uint64_t)];
    if (std::fread(start, 5 + 2 * sizeof(uint64_t), 1, f) != 1)
        return ErrorSysErrno;
    uint64_t compCount;
    uint64_t dimCount;
    std::memcpy(&compCount, start + 5, sizeof(uint64_t));
    std::memcpy(&dimCount, start + 5 + sizeof(uint64_t), sizeof(uint64_t));
    if (start[0] != 'T' || start[1] != 'A' || start[2] != 'D' || start[3] != 0
            || start[4] > 15
            || compCount > std::numeric_limits<size_t>::max()
            || dimCount > std::numeric_limits<size_t>::max()) {
        return ErrorInvalidData;
    }
    std::vector<size_t> dimensions(dimCount);
    if (sizeof(uint64_t) == sizeof(size_t)) {
        if (std::fread(dimensions.data(), sizeof(size_t), dimCount, f) != dimCount)
            return ErrorSysErrno;
    } else {
        std::vector<uint64_t> origDimensions(dimCount);
        if (std::fread(origDimensions.data(), sizeof(uint64_t), dimCount, f) != dimCount)
            return ErrorSysErrno;
        for (size_t d = 0; d < dimCount; d++)
            dimensions[d] = origDimensions[d];
    }

    array = ArrayContainer(dimensions, compCount, static_cast<Type>(start[4]));
    Error e;
    if ((e = readTadTagList(f, array.globalTagList())) != ErrorNone)
        return e;
    for (size_t c = 0; c < array.componentCount(); c++)
        if ((e = readTadTagList(f, array.componentTagList(c))) != ErrorNone)
            return e;
    for (size_t d = 0; d < array.dimensionCount(); d++)
        if ((e = readTadTagList(f, array.dimensionTagList(d))) != ErrorNone)
            return e;
    return ErrorNone;
}

static bool readTadData(FILE *f, ArrayContainer& array)
{
    return (std::fread(array.data(), array.dataSize(), 1, f) == 1);
}

static bool skipTadData(FILE *f, const ArrayContainer& array)
{
    return (fseeko(f, array.dataSize(), SEEK_CUR) == 0 ? true : false);
}

int FormatImportExportTAD::arrayCount()
{
    if (_arrayCount >= -1)
        return _arrayCount;

    if (!_f)
        return -1;

    // find offsets of all TADs in the file
    off_t curPos = ftello(_f);
    if (curPos < 0) {
        _arrayCount = -1;
        return _arrayCount;
    }
    rewind(_f);
    while (hasMore()) {
        off_t arrayPos = ftello(_f);
        if (arrayPos < 0) {
            _arrayOffsets.clear();
            _arrayCount = -1;
            return -1;
        }
        ArrayContainer array;
        Error e = readTadHeader(_f, array);
        if (e != ErrorNone || !skipTadData(_f, array))
            return -1;
        _arrayOffsets.push_back(arrayPos);
        if (_arrayOffsets.size() == size_t(std::numeric_limits<int>::max()) && hasMore()) {
            _arrayOffsets.clear();
            _arrayCount = -1;
            return -1;
        }
    }
    if (fseeko(_f, curPos, SEEK_SET) < 0) {
        _arrayOffsets.clear();
        _arrayCount = -1;
        return -1;
    }
    _arrayCount = _arrayOffsets.size();
    return _arrayCount;
}

ArrayContainer FormatImportExportTAD::readArray(Error* error, int arrayIndex)
{
    // Seek if necessary
    if (arrayIndex >= 0) {
        if (arrayCount() < 0) {
            *error = ErrorSeekingNotSupported;
            return ArrayContainer();
        }
        if (arrayIndex >= arrayCount()) {
            *error = ErrorInvalidData;
            return ArrayContainer();
        }
        if (fseeko(_f, _arrayOffsets[arrayIndex], SEEK_SET) < 0) {
            *error = ErrorSysErrno;
            return ArrayContainer();
        }
    }

    // Read the TAD header
    ArrayContainer array;
    Error e = readTadHeader(_f, array);
    if (e != ErrorNone) {
        *error = e;
        return ArrayContainer();
    }

    // Read the data
    if (!readTadData(_f, array)) {
        e = ErrorSysErrno;
    }
    if (e != ErrorNone) {
        *error = e;
        return ArrayContainer();
    }

    // Return the array
    return array;
}

bool FormatImportExportTAD::hasMore()
{
    int c = fgetc(_f);
    if (c == EOF) {
        return false;
    } else {
        ungetc(c, _f);
        return true;
    }
}

Error FormatImportExportTAD::writeArray(const ArrayContainer& array)
{
    return (writeTad(_f, array) ? ErrorNone : ErrorSysErrno);
}

}
