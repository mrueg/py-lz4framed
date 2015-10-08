/* Copyright (c) 2015 Iotic Labs Ltd. All rights reserved. */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// byte/string argument parsing size as Py_ssize_t (e.g. via PyArg_ParseTupleAndKeywords)
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <bytesobject.h>

#include "lz4frame_static.h"

/******************************************************************************/

#define UNUSED(x) (void)(x)
#define MAX(x, y) (x) >= (y) ? (x) : (y)
#define KB *(1<<10)
#define MB *(1<<20)

#define BAIL_ON_LZ4_ERROR(code) {\
    size_t __err = (code);\
    if (LZ4F_isError(__err)) {\
        PyObject *num = NULL, *str = NULL, *tuple = NULL;\
        if ((num = PyLong_FromSize_t(-__err)) &&\
            (str = PyUnicode_FromString(LZ4F_getErrorName(__err))) &&\
            (tuple = PyTuple_Pack(2, str, num))) {\
            PyErr_SetObject(LZ4FError, tuple);\
        /* backup method in case object creation fails */\
        } else {\
            Py_XDECREF(tuple);\
            PyErr_Format(LZ4FError, "[%zd] %s", __err, LZ4F_getErrorName(__err));\
        }\
        Py_XDECREF(num);\
        Py_XDECREF(str);\
        goto bail;\
    }\
}

#define BAIL_ON_NULL(result) \
if (NULL == (result)) {\
    goto bail;\
}

#define BAIL_ON_NONZERO(result) \
if (result) {\
    goto bail;\
}

#define COMPRESSION_CAPSULE_NAME "_lz4fcctx"
#define DECOMPRESSION_CAPSULE_NAME "_lz4fdctx"

PyDoc_STRVAR(__lz4f_error__doc__,
"Raised when an lz4-specific error occurs. Arguments are the error message and associated code.");
static PyObject *LZ4FError;

/******************************************************************************/

static int _valid_lz4f_block_size_id(int id) {
    switch (id) {
        case LZ4F_default:
        case LZ4F_max64KB:
        case LZ4F_max256KB:
        case LZ4F_max1MB:
        case LZ4F_max4MB:
            return 1;
        default:
            return 0;
    }
}

static size_t _lz4f_block_size_from_id(int id) {
    static const size_t blockSizes[4] = { 64 KB, 256 KB, 1 MB, 4 MB };

    if (!_valid_lz4f_block_size_id(id)) {
        return 0;
    }
    if (id == LZ4F_default) {
        id = LZ4F_max64KB;
    }
    id -= 4;
    return blockSizes[id];
}

// TODO - test on 32-bit linux (pi) 2.7 & 3.x
// TODO - test on windows? http://blog.ionelmc.ro/2014/12/21/compiling-python-extensions-on-windows/
// TODO - flush method?

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_get_block_size__doc__,
"get_block_size(id) -> int\n"
"\n"
"Returns block size in bytes for the given lz4 block size id\n"
"\n"
"Args:\n"
"    id (int): One of LZ4F_BLOCKSIZE_* constants, e.g. retrieved via get_frame_info()\n");
#define FUNC_DEF_GET_BLOCK_SIZE {"get_block_size", (PyCFunction)_lz4framed_get_block_size, METH_VARARGS,\
                                 _lz4framed_get_block_size__doc__}
static PyObject*
_lz4framed_get_block_size(PyObject *self, PyObject *args) {
    int block_id;
    PyObject *byte_count = NULL;
    UNUSED(self);

    if (!PyArg_ParseTuple(args, "i:get_block_size", &block_id)) {
        goto bail;
    }
    if (!_valid_lz4f_block_size_id(block_id)) {
        PyErr_Format(PyExc_ValueError, "id (%d) invalid", block_id);
        goto bail;
    }
    BAIL_ON_NULL(byte_count = PyLong_FromSize_t(_lz4f_block_size_from_id(block_id)));

    return byte_count;

bail:
    Py_XDECREF(byte_count);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_compress__doc__,
"compress(b, block_size_id=LZ4F_BLOCKSIZE_DEFAULT, block_mode_linked=True,\n"
"         checksum=False, level=0) -> bytes\n"
"\n"
"Compresses the data given in b, returning the compressed and lz4-framed\n"
"result.\n"
"\n"
"Args:\n"
"    b (bytes): The object containing data to compress\n"
"    block_size_id (int): Compression block size identifier, one of the\n"
"                         LZ4F_BLOCKSIZE_* constants\n"
"    block_mode_linked (bool): Whether compression blocks are linked. Better compression\n"
"                              is achieved in linked mode.\n"
"    checksum (bool): Whether to produce frame checksum\n"
"    level (int): Compression level. Values lower than 3 use fast compression. Recommended\n"
"                 range for hc compression is between 4 and 9, with a maximum of 16.\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a compression failure occured");
#define FUNC_DEF_COMPRESS {"compress", (PyCFunction)_lz4framed_compress, METH_VARARGS | METH_KEYWORDS,\
                           _lz4framed_compress__doc__}
static PyObject*
_lz4framed_compress(PyObject *self, PyObject *args, PyObject *kwargs) {
#if PY_MAJOR_VERSION >= 3
    static const char *format = "y#|iiii:compress";
#else
    static const char *format = "s#|iiii:compress";
#endif
    static char *keywords[] = {"b", "block_size_id", "block_mode_linked", "checksum", "level", NULL};

    LZ4F_preferences_t prefs = {{0}, 0, 0, {0}};
    const char *input;
    Py_ssize_t input_len;
    int block_id = LZ4F_default;
    int block_mode_linked = 1;
    int checksum = 0;
    int compression_level = 0;
    PyObject *output = NULL;
    char * output_str;
    size_t output_len;
    UNUSED(self);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, format, keywords, &input, &input_len, &block_id,
                                     &block_mode_linked, &checksum, &compression_level)) {
        goto bail;
    }
    if (input_len <= 0) {
        PyErr_SetString(PyExc_ValueError, "b (data) is empty");
        goto bail;
    }
    if (!_valid_lz4f_block_size_id(block_id)) {
        PyErr_Format(PyExc_ValueError, "block_size_id (%d) invalid", block_id);
        goto bail;
    }
    if (compression_level < 0 || compression_level > 16) {
        PyErr_Format(PyExc_ValueError, "compression_level (%d) invalid", compression_level);
        goto bail;
    }

    prefs.frameInfo.contentSize = input_len;
    prefs.frameInfo.blockMode = block_mode_linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = block_id;
    prefs.frameInfo.contentChecksumFlag = checksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
    prefs.compressionLevel = compression_level;

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressFrameBound(input_len, &prefs));
    BAIL_ON_NULL(output = PyBytes_FromStringAndSize(NULL, output_len));
    BAIL_ON_NULL(output_str = PyBytes_AsString(output));

    // output length might be shorter than estimated
    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressFrame(output_str, output_len, input, input_len, &prefs));

    BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
    return output;

bail:
    Py_XDECREF(output);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_decompress__doc__,
"decompress(b, buffer_size=1024) -> bytes\n"
"\n"
"Decompresses framed lz4 blocks from the data given in *b*, returning the\n"
"uncompressed result. For large payloads consider using Decompressor class\n"
"to decompress in chunks.\n"
"\n"
"Args:\n"
"    b (bytes): The object containing lz4-framed data to decompress\n"
"    buffer_size (int): Initial size of buffer in bytes for decompressed\n"
"                       result. This is useful if the frame is not expected\n"
"                       to indicate uncompressed length of data. If\n"
"                       buffer_size is not large enough, it will be doubled\n"
"                       until the resulting data fits. If the frame states\n"
"                       uncompressed size or if len(b) > buffer_size, this\n"
"                       parameter is ignored.\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a decompression failure occured");
#define FUNC_DEF_DECOMPRESS {"decompress", (PyCFunction)_lz4framed_decompress, METH_VARARGS | METH_KEYWORDS,\
                             _lz4framed_decompress__doc__}
static PyObject*
_lz4framed_decompress(PyObject *self, PyObject *args, PyObject *kwargs) {
#if PY_MAJOR_VERSION >= 3
    static const char *format = "y#|i:decompress";
#else
    static const char *format = "s#|i:decompress";
#endif
    static char *keywords[] = {"b", "buffer_size", NULL};

    LZ4F_decompressionContext_t ctx = NULL;
    LZ4F_decompressOptions_t opt = {0, {0}};
    LZ4F_frameInfo_t frame_info;
    const char *input_pos;          // position in input
    size_t input_read;              // used by LZ4 functions to indicate how many bytes were / can be read
    size_t input_remaining;         // bytes remaining in input
    size_t input_size_hint;         // LZ4 hint to how many bytes make up the remaining block + next header
    int buffer_size = 1024;
    PyObject *output = NULL;
    char *output_pos;               // position in output
    size_t output_len;              // size of output
    size_t output_remaining;        // bytes still available in output
    size_t output_written;          // used by LZ4 to indicate how many bytes were / can be written
    UNUSED(self);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, format, keywords, &input_pos, &input_remaining, &buffer_size)) {
        goto bail;
    }
    if (input_remaining <= 0) {
        PyErr_Format(PyExc_ValueError, "b (data) is empty");
        goto bail;
    }
    if (buffer_size <= 0) {
        PyErr_Format(PyExc_ValueError, "buffer_size (%d) invalid", buffer_size);
        goto bail;
    }
    input_read = input_remaining;

    BAIL_ON_LZ4_ERROR(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION));

    // retrieve uncompressed data size
    BAIL_ON_LZ4_ERROR(input_size_hint = LZ4F_getFrameInfo(ctx, &frame_info, input_pos, &input_read));
    input_pos += input_read;
    input_remaining = input_read = input_remaining - input_read;
    if (frame_info.contentSize > 0) {
        output_len = frame_info.contentSize;
        // Prevent LZ4 from buffering output - works if uncompressed size known since output does not have to be resized
        opt.stableDst = 1;
    } else {
        // uncompressed size is always at least that of compressed
        output_len = MAX((size_t) buffer_size, input_remaining);
    }

    // set up initial output buffer
    BAIL_ON_NULL(output = PyBytes_FromStringAndSize(NULL, output_len));
    BAIL_ON_NULL(output_pos = PyBytes_AsString(output));
    output_remaining = output_written = output_len;

    while (input_size_hint) {
        // decompress next chunk
        BAIL_ON_LZ4_ERROR(input_size_hint = LZ4F_decompress(ctx, output_pos, &output_written, input_pos, &input_read,
                                                            &opt));
        output_pos += output_written;
        output_remaining = output_written = output_remaining - output_written;
        input_pos += input_read;
        input_remaining = input_read = input_remaining - input_read;

        // decompression complete
        if (!input_size_hint) {
            output_len -= output_remaining;
            break;
        }
        // destination too small
        if (input_remaining > 0) {
            if (frame_info.contentSize > 0) {
                // if frame specifies size, should never have to enlarge
                BAIL_ON_NONZERO(PyErr_WarnEx(PyExc_RuntimeWarning, "lz4frame contentSize mismatch", 2));
            }
            output_remaining = output_written += output_len;
            output_len *= 2;
            BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
            BAIL_ON_NULL(output_pos = PyBytes_AsString(output));
            output_pos += (output_len - output_remaining);
        }
    }
    BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
    LZ4F_freeDecompressionContext(ctx);

    return output;

bail:
    Py_XDECREF(output);
    LZ4F_freeDecompressionContext(ctx);
    return NULL;
}

/******************************************************************************/

static void _cctx_capsule_destructor(PyObject *py_ctx) {
    LZ4F_compressionContext_t ctx = (LZ4F_compressionContext_t)PyCapsule_GetPointer(py_ctx, COMPRESSION_CAPSULE_NAME);
    if (NULL != ctx) {
        // ignoring errors here since shouldn't throw exception in destructor
        LZ4F_freeCompressionContext(ctx);
    }
}

static void _dctx_capsule_destructor(PyObject *py_ctx) {
    LZ4F_decompressionContext_t ctx = (LZ4F_decompressionContext_t)PyCapsule_GetPointer(py_ctx,
                                                                                        DECOMPRESSION_CAPSULE_NAME);
    if (NULL != ctx) {
        // ignoring errors here since shouldn't throw exception in destructor
        LZ4F_freeDecompressionContext(ctx);
    }
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_create_compression_context__doc__,
"create_compression_context() -> PyCapsule\n"
"\n"
"Create compression context for use in chunked compression.\n");
#define FUNC_DEF_CREATE_CCTX {"create_compression_context", _lz4framed_create_compression_context, METH_NOARGS,\
                              _lz4framed_create_compression_context__doc__}
static PyObject*
_lz4framed_create_compression_context(PyObject *self, PyObject *args) {
    LZ4F_compressionContext_t ctx = NULL;
    UNUSED(self);
    UNUSED(args);

    BAIL_ON_LZ4_ERROR(LZ4F_createCompressionContext(&ctx, LZ4F_VERSION));
    return PyCapsule_New(ctx, COMPRESSION_CAPSULE_NAME, _cctx_capsule_destructor);

bail:
    // must NOT be freed once capsule exists (since destructor responsible for freeing)
    LZ4F_freeCompressionContext(ctx);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_create_decompression_context__doc__,
"create_decompression_context() -> PyCapsule\n"
"\n"
"Create decompression context for use in chunked decompression.\n");
#define FUNC_DEF_CREATE_DCTX {"create_decompression_context", _lz4framed_create_decompression_context, METH_NOARGS,\
                              _lz4framed_create_decompression_context__doc__}
static PyObject*
_lz4framed_create_decompression_context(PyObject *self, PyObject *args) {
    LZ4F_decompressionContext_t ctx = NULL;
    UNUSED(self);
    UNUSED(args);

    BAIL_ON_LZ4_ERROR(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION));
    return PyCapsule_New(ctx, DECOMPRESSION_CAPSULE_NAME, _dctx_capsule_destructor);

bail:
    // must NOT be freed once capsule exists (since destructor responsible for freeing)
    LZ4F_freeDecompressionContext(ctx);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_compress_begin__doc__,
"compress_begin(ctx, block_size_id=LZ4F_BLOCKSIZE_DEFAULT, block_mode_linked=True,\n"
"               checksum=False, autoflush=False, level=0) -> bytes\n"
"\n"
"Generates and returns frame header, sets compression options.\n"
"\n"
"Args:\n"
"    ctx: Compression context\n"
"    block_size_id (int): Compression block size identifier, one of the\n"
"                         LZ4F_BLOCKSIZE_* constants. Use get_block_size() to\n"
"                         determine size in bytes.\n"
"    block_mode_linked (bool): Whether compression blocks are linked\n"
"    checksum (bool): Whether to produce frame checksum\n"
"    autoflush (bool): Whether to flush output on update() calls rather than buffering\n"
"                      incomplete blocks internally.\n"
"    level (int): Compression level. Values lower than 3 use fast compression. Recommended\n"
"                 range for hc compression is between 4 and 9, with a maximum of 16.\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a compression failure occured");
#define FUNC_DEF_COMPRESS_BEGIN {"compress_begin", (PyCFunction)_lz4framed_compress_begin,\
                                 METH_VARARGS | METH_KEYWORDS, _lz4framed_compress_begin__doc__}
static PyObject*
_lz4framed_compress_begin(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *format = "O|iiiii:compress_begin";
    static char *keywords[] = {"ctx", "block_size_id", "block_mode_linked", "checksum", "autoflush", "level", NULL};

    LZ4F_preferences_t prefs = {{0}, 0, 0, {0}};
    LZ4F_compressionContext_t ctx;
    PyObject *ctx_capsule;
    int block_id = LZ4F_default;
    int block_mode_linked = 1;
    int checksum = 0;
    int autoflush = 0;
    int compression_level = 0;
    PyObject *output = NULL;
    char *output_str;
    size_t output_len = 15;  // maximum header size
    UNUSED(self);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, format, keywords, &ctx_capsule, &block_id, &block_mode_linked,
                                     &checksum, &autoflush, &compression_level)) {
        goto bail;
    }
    if (!PyCapsule_IsValid(ctx_capsule, COMPRESSION_CAPSULE_NAME)) {
        PyErr_SetString(PyExc_ValueError, "ctx invalid");
        goto bail;
    }
    if (!_valid_lz4f_block_size_id(block_id)) {
        PyErr_Format(PyExc_ValueError, "block_size_id (%d) invalid", block_id);
        goto bail;
    }
    if (compression_level < 0 || compression_level > 16) {
        PyErr_Format(PyExc_ValueError, "compression_level (%d) invalid", compression_level);
        goto bail;
    }

    // Guaranteed to succeed due to PyCapsule_IsValid check above
    ctx = PyCapsule_GetPointer(ctx_capsule, COMPRESSION_CAPSULE_NAME);

    prefs.frameInfo.blockMode = block_mode_linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = block_id;
    prefs.frameInfo.contentChecksumFlag = checksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
    prefs.compressionLevel = compression_level;
    prefs.autoFlush = autoflush ? 1 : 0;

    BAIL_ON_NULL(output = PyBytes_FromStringAndSize(NULL, output_len));
    BAIL_ON_NULL(output_str = PyBytes_AsString(output));

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressBegin(ctx, output_str, output_len, &prefs));

    BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
    return output;

bail:
    Py_XDECREF(output);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_compress_update__doc__,
"compress_update(ctx, b) -> bytes\n"
"\n"
"Compresses and returns the given data. Note: return can be zero-length if autoflush\n"
"parameter is not set via compress_begin(). Once all data has been compressed,\n"
"compress_end() must be called (to flush any remaining data and finalise frame.\n"
"\n"
"Args:\n"
"    ctx: Compression context\n"
"    b (bytes): The object containing lz4-framed data to decompress\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a compression failure occured");
#define FUNC_DEF_COMPRESS_UPDATE {"compress_update", (PyCFunction)_lz4framed_compress_update, METH_VARARGS,\
                                  _lz4framed_compress_update__doc__}
static PyObject*
_lz4framed_compress_update(PyObject *self, PyObject *args) {
#if PY_MAJOR_VERSION >= 3
    static const char *format = "Oy#:compress_update";
#else
    static const char *format = "Os#:compress_update";
#endif
    LZ4F_compressionContext_t ctx;
    PyObject *ctx_capsule;
    const char *input;
    Py_ssize_t input_len;
    PyObject *output = NULL;
    char *output_str;
    size_t output_len;
    UNUSED(self);

    if (!PyArg_ParseTuple(args, format, &ctx_capsule, &input, &input_len)) {
        goto bail;
    }
    if (!PyCapsule_IsValid(ctx_capsule, COMPRESSION_CAPSULE_NAME)) {
        PyErr_SetString(PyExc_ValueError, "ctx invalid");
        goto bail;
    }
    if (input_len <= 0) {
        PyErr_SetString(PyExc_ValueError, "b (data) is empty");
        goto bail;
    }
    // Guaranteed to succeed due to PyCapsule_IsValid check above
    ctx = PyCapsule_GetPointer(ctx_capsule, COMPRESSION_CAPSULE_NAME);

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressBound(input_len, NULL));
    BAIL_ON_NULL(output = PyBytes_FromStringAndSize(NULL, output_len));
    BAIL_ON_NULL(output_str = PyBytes_AsString(output));

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressUpdate(ctx, output_str, output_len, input, input_len, NULL));

    BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
    return output;

bail:
    Py_XDECREF(output);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_compress_end__doc__,
"compress_end(ctx) -> bytes\n"
"\n"
"Flushes any remaining compressed data, finalises frame and returns said data. After\n"
"successful decompressino the context can be re-used for another frame.\n"
"\n"
"Args:\n"
"    ctx: Compression context\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a compression failure occured");
#define FUNC_DEF_COMPRESS_END {"compress_end", (PyCFunction)_lz4framed_compress_end, METH_O,\
                               _lz4framed_compress_end__doc__}
static PyObject*
_lz4framed_compress_end(PyObject *self, PyObject *arg) {
    LZ4F_compressionContext_t ctx;

    PyObject *output = NULL;
    char *output_str;
    size_t output_len;
    UNUSED(self);

    if (!PyCapsule_IsValid(arg, COMPRESSION_CAPSULE_NAME)) {
        PyErr_SetString(PyExc_ValueError, "ctx invalid");
        goto bail;
    }
    // Guaranteed to succeed due to PyCapsule_IsValid check above
    ctx = PyCapsule_GetPointer(arg, COMPRESSION_CAPSULE_NAME);

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressBound(0, NULL));
    BAIL_ON_NULL(output = PyBytes_FromStringAndSize(NULL, output_len));
    BAIL_ON_NULL(output_str = PyBytes_AsString(output));

    BAIL_ON_LZ4_ERROR(output_len = LZ4F_compressEnd(ctx, output_str, output_len, NULL));

    BAIL_ON_NONZERO(_PyBytes_Resize(&output, output_len));
    return output;

bail:
    Py_XDECREF(output);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_get_frame_info__doc__,
"get_frame_info(ctx) -> dict\n"
"\n"
"Retrieves frame header information. This method can be called at any point during the\n"
"decompression process. If the header has not been parsed yet due to lack of data, one can\n"
"expect an Lz4FramedError exception with error code LZ4F_ERROR_HEADER_INCOMPLETE. On success\n"
"the method returns a dict with the following keys:\n"
"    input_hint (int)         - How many bytes to provide to next decompress() call for optimal\n"
"                               performance (due to not having to use internal buffers\n"
"    length (int)             - Uncompressed length of data (or zero if unknown)\n"
"    block_size_id (int)      - One of LZ4F_BLOCKSIZE_* constants\n"
"    block_mode_linked (bool) - Whether blocks in frame are linked\n"
"    checksum (bool)          - Whether the frame has a checksum (which will be verified)\n"
"\n"
"Args:\n"
"    ctx: Decompression context\n"
"\n"
"Raises:\n"
"    Lz4FramedError: If a compression failure occured");
#define FUNC_DEF_GET_FRAME_INFO {"get_frame_info", (PyCFunction)_lz4framed_get_frame_info, METH_O,\
                                 _lz4framed_get_frame_info__doc__}
static PyObject*
_lz4framed_get_frame_info(PyObject *self, PyObject *arg) {
    LZ4F_decompressionContext_t ctx;
    LZ4F_frameInfo_t frameInfo;
    size_t input_hint;
    size_t input_read = 0;
    PyObject *dict = NULL;
    PyObject *item = NULL;
    UNUSED(self);

    if (!PyCapsule_IsValid(arg, DECOMPRESSION_CAPSULE_NAME)) {
        PyErr_SetString(PyExc_ValueError, "ctx invalid");
        goto bail;
    }
    // Guaranteed to succeed due to PyCapsule_IsValid check above
    ctx = PyCapsule_GetPointer(arg, DECOMPRESSION_CAPSULE_NAME);

    BAIL_ON_LZ4_ERROR(input_hint = LZ4F_getFrameInfo(ctx, &frameInfo, NULL, &input_read));

    BAIL_ON_NULL(dict = PyDict_New());
    BAIL_ON_NULL(item = PyLong_FromSize_t(input_hint));
    BAIL_ON_NONZERO(PyDict_SetItemString(dict, "input_hint", item));
    Py_CLEAR(item);
    BAIL_ON_NULL(item = PyLong_FromUnsignedLongLong(frameInfo.contentSize));
    BAIL_ON_NONZERO(PyDict_SetItemString(dict, "length", item));
    Py_CLEAR(item);
    BAIL_ON_NULL(item = PyLong_FromLong(frameInfo.blockSizeID));
    BAIL_ON_NONZERO(PyDict_SetItemString(dict, "block_size_id", item));
    Py_CLEAR(item);
    BAIL_ON_NULL(item = PyBool_FromLong(frameInfo.blockMode == LZ4F_blockLinked));
    BAIL_ON_NONZERO(PyDict_SetItemString(dict, "block_mode_linked", item));
    Py_CLEAR(item);
    BAIL_ON_NULL(item = PyBool_FromLong(frameInfo.contentChecksumFlag == LZ4F_contentChecksumEnabled));
    BAIL_ON_NONZERO(PyDict_SetItemString(dict, "checksum", item));
    Py_CLEAR(item);

    return dict;

bail:
    // necessary for item if dict assignment fails
    Py_XDECREF(item);
    Py_XDECREF(dict);
    return NULL;
}

/******************************************************************************/

PyDoc_STRVAR(_lz4framed_decompress_update__doc__,
"decompress(ctx, b, chunk_len=65536) -> list\n"
"\n"
"Decompresses parts of an lz4 frame from data given in *b*, returning the\n"
"uncompressed result as a list of chunks, with the last element being input_hint\n"
"(i.e. how many bytes to ideally expect on the next call). Once input_hint is\n"
"zero, decompression of the whole frame is complete. Note: Some calls to this\n"
"function may return no chunks if they are incomplete.\n"
"Args:\n"
"    ctx: Decompression context\n"
"    b (bytes): The object containing lz4-framed data to decompress\n"
"    chunk_len (int): Size of uncompressed chunks in bytes. If not all of the\n"
"                     data fits in one chunk, multiple will be used. Ideally\n"
"                     only one chunk is required per call of this method - this can\n"
"                     be determined from block_size_id via get_frame_info() call."
"\n"
"Raises:\n"
"    Lz4FramedError: If a decompression failure occured");
#define FUNC_DEF_DECOMPRESS_UPDATE {"decompress_update", (PyCFunction)_lz4framed_decompress_update,\
                                    METH_VARARGS | METH_KEYWORDS, _lz4framed_decompress_update__doc__}
static PyObject*
_lz4framed_decompress_update(PyObject *self, PyObject *args, PyObject *kwargs) {
#if PY_MAJOR_VERSION >= 3
    static const char *format = "Oy#|i:decompress_update";
#else
    static const char *format = "Os#|i:decompress_update";
#endif
    static char *keywords[] = {"ctx", "b", "chunk_len", NULL};

    LZ4F_decompressionContext_t ctx;
    PyObject *ctx_capsule;
    const char *input_pos;           // position in input
    size_t input_remaining;          // bytes remaining in input
    size_t input_read;               // used by LZ4 functions to indicate how many bytes were / can be read
    size_t input_size_hint = 1;      // LZ4 hint to how many bytes make up the remaining block + next header
    size_t chunk_len = 65536;        // size of chunks
    PyObject *list = NULL;           // function return
    PyObject *size_hint = NULL;      // python object of input_size_hint
    PyObject *chunk = NULL ;
    char *chunk_pos = NULL ;         // position in current chunk
    size_t chunk_remaining;          // space remaining in chunk
    size_t chunk_written;            // used by lz4 to indicate how much has been written
    UNUSED(self);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, format, keywords, &ctx_capsule, &input_pos, &input_remaining,
                                     &chunk_len)) {
        goto bail;
    }
    if (!PyCapsule_IsValid(ctx_capsule, DECOMPRESSION_CAPSULE_NAME)) {
        PyErr_SetString(PyExc_ValueError, "ctx invalid");
        goto bail;
    }
    if (input_remaining <= 0) {
        PyErr_SetString(PyExc_ValueError, "b (data) is empty");
        goto bail;
    }
    if (chunk_len <= 0) {
        PyErr_SetString(PyExc_ValueError, "chunk_len invalid");
        goto bail;
    }
    // Guaranteed to succeed due to PyCapsule_IsValid check above
    ctx = PyCapsule_GetPointer(ctx_capsule, DECOMPRESSION_CAPSULE_NAME);

    input_read = input_remaining;

    // output list
    BAIL_ON_NULL(list = PyList_New(0));

    // first chunk
    BAIL_ON_NULL(chunk = PyBytes_FromStringAndSize(NULL, chunk_len));
    BAIL_ON_NULL(chunk_pos = PyBytes_AsString(chunk));
    chunk_remaining = chunk_written = chunk_len;

    while (input_remaining > 0 && input_size_hint > 0) {
        if (chunk_remaining <= 0) {
            // append previous (full) chunk to list
            BAIL_ON_NONZERO(PyList_Append(list, chunk));
            Py_CLEAR(chunk);
            BAIL_ON_NULL(chunk = PyBytes_FromStringAndSize(NULL, chunk_len));
            BAIL_ON_NULL(chunk_pos = PyBytes_AsString(chunk));
            chunk_remaining = chunk_written = chunk_len;
        }
        BAIL_ON_LZ4_ERROR(input_size_hint = LZ4F_decompress(ctx, chunk_pos, &chunk_written, input_pos, &input_read,
                                                            NULL));
        chunk_pos += chunk_written;
        chunk_remaining = chunk_written = chunk_remaining - chunk_written;
        input_pos += input_read;
        input_remaining = input_read = input_remaining - input_read;
    }
    // append & reduce size of final chunk (if contains any data)
    if (chunk_remaining < chunk_len) {
        BAIL_ON_NONZERO(_PyBytes_Resize(&chunk, chunk_len - chunk_remaining));
        BAIL_ON_NONZERO(PyList_Append(list, chunk));
        Py_CLEAR(chunk);
    }
    // append input size hint to list
    BAIL_ON_NULL(size_hint = PyLong_FromSize_t(input_size_hint));
    BAIL_ON_NONZERO(PyList_Append(list, size_hint));
    Py_CLEAR(size_hint);

    return list;

bail:
    Py_XDECREF(chunk);
    Py_XDECREF(size_hint);
    Py_XDECREF(list);
    return NULL;
}


/******************************************************************************/

static PyMethodDef Lz4framedMethods[] = {
    FUNC_DEF_GET_BLOCK_SIZE, FUNC_DEF_COMPRESS, FUNC_DEF_DECOMPRESS, FUNC_DEF_CREATE_CCTX, FUNC_DEF_CREATE_DCTX,
    FUNC_DEF_COMPRESS_BEGIN, FUNC_DEF_COMPRESS_UPDATE, FUNC_DEF_COMPRESS_END, FUNC_DEF_GET_FRAME_INFO,
    FUNC_DEF_DECOMPRESS_UPDATE,
    {NULL, NULL, 0, NULL}
};

struct module_state {
    PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
static struct module_state _state;
#endif

#if PY_MAJOR_VERSION >= 3

static int myextension_traverse(PyObject *m, visitproc visit, void *arg) {
    Py_VISIT(GETSTATE(m)->error);
    return 0;
}

static int myextension_clear(PyObject *m) {
    Py_CLEAR(GETSTATE(m)->error);
    return 0;
}


static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "_lz4framed",
        NULL,
        sizeof(struct module_state),
        Lz4framedMethods,
        NULL,
        myextension_traverse,
        myextension_clear,
        NULL
};

#define INITERROR return NULL
PyObject*
PyInit__lz4framed(void)

#else
#define INITERROR return

void
init_lz4framed(void)
#endif
{
    struct module_state *state = NULL;
#if PY_MAJOR_VERSION >= 3
    PyObject *module = PyModule_Create(&moduledef);
#else
    PyObject *module = Py_InitModule("_lz4framed", Lz4framedMethods);
#endif

    BAIL_ON_NULL(module);
    BAIL_ON_NULL(state = GETSTATE(module));

    BAIL_ON_NULL(state->error = PyErr_NewExceptionWithDoc("_lz4framed.Lz4FramedError", __lz4f_error__doc__, NULL,
                                                          NULL));
    LZ4FError = state->error;
    Py_INCREF(LZ4FError);

    // non-zero returns indicate error
    if (PyModule_AddObject(module, "Lz4FramedError", LZ4FError) ||
        PyModule_AddStringConstant(module, "__version__", VERSION) ||
        // defined at compile time, is of the form "r123"
        PyModule_AddStringConstant(module, "LZ4_VERSION", LZ4_VERSION) ||
        PyModule_AddIntMacro(module, LZ4F_VERSION) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_GENERIC) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_maxBlockSize_invalid) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_blockMode_invalid) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_contentChecksumFlag_invalid) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_compressionLevel_invalid) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_headerVersion_wrong) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_blockChecksum_unsupported) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_reservedFlag_set) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_allocation_failed) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_srcSize_tooLarge) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_dstMaxSize_tooSmall) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_frameHeader_incomplete) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_frameType_unknown) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_frameSize_wrong) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_srcPtr_wrong) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_decompressionFailed) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_headerChecksum_invalid) ||
        PyModule_AddIntMacro(module, LZ4F_ERROR_contentChecksum_invalid) ||
        PyModule_AddIntConstant(module, "LZ4F_BLOCKSIZE_DEFAULT", LZ4F_default) ||
        PyModule_AddIntConstant(module, "LZ4F_BLOCKSIZE_MAX64KB", LZ4F_max64KB) ||
        PyModule_AddIntConstant(module, "LZ4F_BLOCKSIZE_MAX256KB", LZ4F_max256KB) ||
        PyModule_AddIntConstant(module, "LZ4F_BLOCKSIZE_MAX1M", LZ4F_max1MB) ||
        PyModule_AddIntConstant(module, "LZ4F_BLOCKSIZE_MAX4M", LZ4F_max4MB)) {
        goto bail;
    }

#if PY_MAJOR_VERSION >= 3
    return module;
#else
    return;
#endif

bail:
    Py_XDECREF(module);
    INITERROR;
}
