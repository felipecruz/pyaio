from __future__ import absolute_import
import os
import pyaio
import gevent
from gevent.event import AsyncResult
from gevent.coros import RLock

def _keep_awake():
    while True:
        gevent.sleep(0.001)

class aioFile(object):
    """a buffered File like object that uses pyaio and gevent"""
    def __init__(self, filename, mode='r', buffer=16<<10):
        modes = os.O_LARGEFILE | os.O_CREAT
        self._offset = 0
        self._buffer_size = buffer
        if buffer:
            self._buffer_lock = RLock()
        self._read = False
        self._write = False
        self._read_buf = None
        self._write_buf = None
        self._eof = False   # Optimization to limit calls
        self._append = False   # Append Mode writes ignore offset
        self._stay_alive = gevent.spawn(_keep_awake);
        if mode.startswith('r') or '+' in mode:
            self._read = True
            self._read_buf = bytearray()
            if '+' not in mode:
                modes |= os.O_RDONLY
        if mode.startswith('w') or mode.startswith('a') or '+' in mode:
            if mode.startswith('w'):
                modes |= os.O_TRUNC
            self._write = True
            self._write_buf = bytearray()
            self._flush = False
            if '+' not in mode:
                modes |= os.O_WRONLY
        if '+' in mode:
            modes |= os.O_RDWR
        if mode.startswith('a'):
            modes |= os.O_APPEND
            self._append = True
        self._fd = os.open(filename, modes)

    def _clear_read_buf(self):
        if self._read:
            self._eof = False
            del self._read_buf[0:]

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def close(self):
        self.flush()
        os.close(self._fd)
        self._stay_alive.kill()

    def stat(self):
        return os.fstat(self._fd)

    def seek(self, pos, how=os.SEEK_SET):
        """Change the file pos, will clear read cache and flush writes """ \
        """This will also clear the EOF flag for the file"""
        offset = self._offset
        if how != os.SEEK_CUR and how != os.SEEK_END and how != os.SEEK_SET:
            raise OSError(14,
                'Invalid seek point use os.SEEK_SET, os.SEEK_CUR, os.SEEK_END')
        if how == os.SEEK_CUR:
            offset += pos
        elif how == os.SEEK_END:
            #Ugh this could be harry if we have outstanding writes
            offset = self.stat().st_size + pos
        else:
            offset = pos
        if offset < 0:
            raise OSError(14, 'File Position invalid, less than 0')
        #Even if the pos didn't change fix the buffers and EOF
        self._clear_read_buf()
        if not self._append:   # DON'T FLUSH on seek with append
            self.flush()
        self._offset = offset
        return offset

    def flush(self):
        """Flush write buffer"""
        if self._write and self._buffer_size:
            self._flush = True
            while len(self._write_buf):
                self.write(None)
            self._flush = False

    def _read_file(self):
        fbuf = bytearray()
        while True:
            part = self.read(16 << 10)  # Read 16k
            if part is None:  # EOF
                break
            fbuf.extend(part)
        return fbuf

    def write(self, buf, offset=None):
        """write a buffer object to file"""
        if not self._write:
            raise IOError(9, 'Bad file descriptor')
        if not self._append and self._buffer_size and self._read_buf:
                # We should clear read cache
            self._clear_read_buf()
        if offset is None:
            offset = self._offset
        write_size = self._buffer_size
        if not self._buffer_size and buf:
            write_size = len(buf)
        if not self._append and offset != self._offset:
            self.seek(offset)  # Makes sure we write our buffer

        #If we buffer we use the global buffer if not we use a local buffer
        if self._buffer_size:
            lbuf = self._write_buf
            self._buffer_lock.acquire()
            if buf:
                                          # The a memoryview of the buffer
                    lbuf.extend(buf)      # pushed to pyaio so we need to lock
        else:
            lbuf = buf

        while lbuf and len(lbuf) >= self._buffer_size \
                or (self._flush and lbuf):
            result = AsyncResult()
            def _write_results(rcode, errno):
                result.set((rcode, errno))
            pyaio.aio_write(self._fd, memoryview(lbuf)[0:write_size],
                            offset, _write_results)
            rcode, errno = result.get()  #SLEEP

            if rcode < 0:   # Some kind of error
                raise IOError(errno, 'AIO Write Error %d' % errno)
            # Clean up buffer (of actually written bytes)
            if self._buffer_size:
                del lbuf[0:rcode]
            else:
                lbuf = None
            self._offset = offset = offset + rcode  # Move the file offset
        if self._buffer_size:
            self._buffer_lock.release()
        if buf:
            return len(buf)
        else:
            return 0

    def read(self, size=0, offset=None):
        """read a size of bytes from the file, or entire file if 0 """ \
        """for speed we assume EOF after first short read"""
        if not self._read:
            raise IOError(9, 'Bad file descriptor')
        if not self._append and self._buffer_size and self._write_buf:
            self.flush()
        if offset is None:
            offset = self._offset
        if offset != self._offset:
            self.seek(offset)  # To make sure we blow away our read cache
        if size == 0:  # Attempt to read entire file and return in a single return
            return self._read_file()
        else:
            rbuf = bytearray()  # Holding Place for multiple reads
            while len(rbuf) < size:  # People get what they ask for
                # If we don't want to buffer then just read what they want
                if len(self._read_buf) < size - len(rbuf) and not self._eof:
                    #Ok we are buffer short so lets do a read
                    result = AsyncResult()
                    def _read_results(buf, rcode, errno):
                        result.set((buf, rcode, errno))
                    read_size = size - len(rbuf)
                    if self._buffer_size:   # If we buffer read buffer instead
                        read_size = self._buffer_size
                    pyaio.aio_read(self._fd, offset, read_size, _read_results)
                    buf, rcode, errno = result.get()  #SLEEP
                    if rcode < 0:  # Some kind of error
                        raise IOError(errno, 'AIO Read Error %d' % errno)
                    #Rcode will be the bytes read so lets push the offset
                    self._offset = offset = offset + rcode
                    if self._buffer_size:
                        self._read_buf.extend(buf)
                    else:
                        rbuf = buf  # Pass through because we are not buffering
                    if rcode == 0 or rcode < read_size:  # Good Enough
                        self._eof = True
                #Do a buffer read
                toread = size - len(rbuf)
                if self._buffer_size:
                    rbuf.extend(memoryview(self._read_buf)[0:toread])
                    #Clean up read buffer
                    del self._read_buf[0:toread]
                if not self._read_buf and self._eof:  # Empty buffer and eof
                    break
            if self._eof and not rbuf:
                return None  #EOF NO DATA
            else:
                return rbuf
