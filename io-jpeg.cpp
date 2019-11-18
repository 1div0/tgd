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

#include <setjmp.h>
#include <jpeglib.h>

#ifdef TAD_WITH_EXIF
# include <libexif/exif-data.h>
# include "io-utils.hpp"
#endif

#include "io-jpeg.hpp"


namespace TAD {

struct my_error_mgr
{
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void my_error_exit(j_common_ptr cinfo)
{
    struct my_error_mgr* my_err = reinterpret_cast<struct my_error_mgr*>(cinfo->err);
    longjmp(my_err->setjmp_buffer, 1);
}

FormatImportExportJPEG::FormatImportExportJPEG() :
    _f(nullptr)
{
}

FormatImportExportJPEG::~FormatImportExportJPEG()
{
    close();
}

Error FormatImportExportJPEG::openForReading(const std::string& fileName, const TagList&)
{
    if (fileName == "-") {
        _f = stdin;
    } else {
        _f = fopen(fileName.c_str(), "rb");
        if (_f)
            _fileName = fileName;
    }
    return _f ? ErrorNone : ErrorSysErrno;
}

Error FormatImportExportJPEG::openForWriting(const std::string& fileName, bool append, const TagList&)
{
    if (append)
        return ErrorFeaturesUnsupported;
    if (fileName == "-") {
        _f = stdout;
    } else {
        _f = fopen(fileName.c_str(), "wb");
        if (_f)
            _fileName = fileName;
    }
    return _f ? ErrorNone : ErrorSysErrno;
}

void FormatImportExportJPEG::close()
{
    if (_f) {
        if (_f != stdin && _f != stdout) {
            fclose(_f);
        }
        _f = nullptr;
    }
    _fileName = std::string();
}

int FormatImportExportJPEG::arrayCount()
{
    return (_f ? 1 : -1);
}

ArrayContainer FormatImportExportJPEG::readArray(Error* error, int arrayIndex)
{
    if (arrayIndex > 0) {
        *error = ErrorSeekingNotSupported;
        return ArrayContainer();
    }
    std::rewind(_f);

    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        *error = ErrorInvalidData;
        return ArrayContainer();
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, _f);
    jpeg_read_header(&cinfo, TRUE);

    ArrayContainer r({cinfo.image_width, cinfo.image_height}, cinfo.num_components, uint8);
    if (cinfo.num_components == 1) {
        r.componentTagList(0).set("INTERPRETATION", "SRGB/GRAY");
    } else {
        r.componentTagList(0).set("INTERPRETATION", "SRGB/R");
        r.componentTagList(1).set("INTERPRETATION", "SRGB/G");
        r.componentTagList(2).set("INTERPRETATION", "SRGB/B");
    }
    jpeg_start_decompress(&cinfo);
    JSAMPROW jrow[1];
    while (cinfo.output_scanline < cinfo.image_height) {
        jrow[0] = static_cast<unsigned char*>(r.get((cinfo.image_height - 1 - cinfo.output_scanline) * cinfo.image_width));
        jpeg_read_scanlines(&cinfo, jrow, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

#if TAD_WITH_EXIF
    ExifData* exifData = exif_data_new_from_file(_fileName.c_str());
    if (exifData) {
        int orientation = 0;
        ExifByteOrder byteOrder = exif_data_get_byte_order(exifData);
        ExifEntry* exifEntry = exif_data_get_entry(exifData, EXIF_TAG_ORIENTATION);
        if (exifEntry)
            orientation = exif_get_short(exifEntry->data, byteOrder);
        exif_data_free(exifData);
        if (orientation >= 1 && orientation <= 8)
            fixImageOrientation(r, static_cast<ImageOriginLocation>(orientation));
    }
#endif

    return r;
}

bool FormatImportExportJPEG::hasMore()
{
    int c = fgetc(_f);
    if (c == EOF) {
        return false;
    } else {
        ungetc(c, _f);
        return true;
    }
}

Error FormatImportExportJPEG::writeArray(const ArrayContainer& array)
{
    if (array.dimensionCount() != 2
            || array.dimension(0) <= 0 || array.dimension(1) <= 0
            || array.dimension(0) > 0x7fffffffL || array.dimension(1) > 0x7fffffffL
            || array.componentType() != uint8
            || (array.componentCount() != 1 && array.componentCount() != 3)) {
        return ErrorFeaturesUnsupported;
    }

    struct jpeg_compress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        return ErrorInvalidData;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, _f);
    cinfo.image_width = array.dimension(0);
    cinfo.image_height = array.dimension(1);
    cinfo.input_components = array.componentCount();
    cinfo.in_color_space = (array.componentCount() == 1 ? JCS_GRAYSCALE : JCS_RGB);
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    std::vector<JSAMPROW> jrows(cinfo.image_height);
    for (unsigned int i = 0; i < cinfo.image_height; i++)
        jrows[i] = static_cast<unsigned char*>(const_cast<void*>(array.get((cinfo.image_height - 1 - i) * cinfo.image_width)));
    jpeg_write_scanlines(&cinfo, jrows.data(), cinfo.image_height);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    if (fflush(_f) != 0) {
        return ErrorSysErrno;
    }
    return ErrorNone;
}

extern "C" FormatImportExport* FormatImportExportFactory_jpeg()
{
    return new FormatImportExportJPEG();
}

}
