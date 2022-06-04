/**
 * Author: Sven Gothel <sgothel@jausoft.com>
 * Copyright (c) 2022 Gothel Software e.K.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
package org.cipherpack;

/**
 * ByteInStream_Feed represents the user side active part of
 * its native C++ implementation.
 *
 * Informational native hooks and the active methods
 * to feed the data into the used instance are exposed.
 */
public final class ByteInStream_Feed implements AutoCloseable  {
    private volatile long nativeInstance;
    /* pp */ long getNativeInstance() { return nativeInstance; }

    /**
     * Construct a ringbuffer backed externally provisioned byte input stream
     * @param id_name arbitrary identifier for this instance
     * @param timeoutMS maximum duration in milliseconds to wait @ check_available() and write(), zero waits infinitely
     */
    public ByteInStream_Feed(final String id_name, final long timeoutMS) {
        if( CPFactory.isInitialized() ) {
            nativeInstance = ctorImpl(id_name, timeoutMS);
        } else {
            System.err.println("ByteInStream_Feed.ctor: CPFactory not initialized, no nativeInstance");
        }
    }
    private native long ctorImpl(final String id_name, final long timeoutMS);

    @Override
    public void close() {
        final long handle;
        synchronized( this ) {
            handle = nativeInstance;
            nativeInstance = 0;
        }
        if( 0 != handle ) {
            dtorImpl(handle);
        }
    }
    private static native void dtorImpl(final long nativeInstance);

    @Override
    public void finalize() {
        close();
    }


    public native boolean end_of_data();

    public native boolean error();

    public native String id();

    public native long get_bytes_read();

    public native boolean has_content_size();

    public native long content_size();

    /**
     * Interrupt a potentially blocked reader.
     *
     * Call this method if intended to abort streaming and to interrupt the reader thread's potentially blocked check_available() call,
     * i.e. done at set_eof()
     *
     * @see set_eof()
     */
    public native void interruptReader();

    /**
     * Write given bytes to the async ringbuffer.
     *
     * Wait up to timeout duration given in constructor until ringbuffer space is available, where fractions_i64::zero waits infinitely.
     *
     * This method is blocking.
     *
     * @param in the byte array to transfer to the async ringbuffer
     * @param offset offset to in byte array to write
     * @param length number of in bytes to write starting at offset
     */
    public native void write(final byte[] in, final int offset, final int length);

    /**
     * Set known content size, informal only.
     * @param content_length the content size in bytes
     */
    public native void set_content_size(final long size);

    /**
     * Set end-of-data (EOS), i.e. when feeder completed provisioning bytes.
     *
     * Implementation issues interruptReader() to unblock a potentially blocked reader thread.
     *
     * @param result should be either -1 for FAILED or 1 for SUCCESS.
     *
     * @see interruptReader()
     */
    public native void set_eof(final int result);

    @Override
    public native String toString();
}
