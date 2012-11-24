from __future__ import absolute_import
import os
import pyaio
from gevent.event import AsyncResult

class aioFile(object):
    """a buffered File like object that uses pyaio and gevent"""
    def __init__(self, filename, mode='r', buffer=16<<10):
        modes = os.O_LARGEFILE | os.O_CREAT
        self._offset = 0
        self._buffer_size = buffer
        self._read = False
        self._write = False
        self._read_buf = None
        self._write_buf = None
        self._eof = False   # Optimization to limit calls

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
        self._fd = os.open(filename, modes)
        if mode.startswith('a'):
            self.seek(0, os.SEEK_END)  # Append so goto end

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
        if self._buffer_size and self._read_buf:  # We should clear read cache
            self._clear_read_buf()
        if offset is None:
            offset = self._offset
        write_size = self._buffer_size
        if not self._buffer_size and buf:
            write_size = len(buf)
        if offset != self._offset:
            self.seek(offset)  # Makes sure we write our buffer
        if buf:
            self._write_buf.extend(buf)
        while len(self._write_buf) >= self._buffer_size \
                or (self._flush and self._write_buf):
            result = AsyncResult()
            def _write_results(rcode, errno):
                result.set((rcode, errno))
            pyaio.aio_write(self._fd, memoryview(self._write_buf)[0:write_size],
                            offset, _write_results)
            rcode, errno = result.get()  # gevent yield
            if rcode < 0:   # Some kind of error
                raise IOError(errno, 'AIO Write Error %d' % errno)
            # Clean up buffer (of actually written bytes)
            del self._write_buf[0:rcode]
            self._offset = offset = offset + rcode  # Move the file offset
        if buf:
            return len(buf)
        else:
            return 0

    def read(self, size=0, offset=None):
        """read a size of bytes from the file, or entire file if 0 """ \
        """for speed we assume EOF after first short read"""
        if not self._read:
            raise IOError(9, 'Bad file descriptor')
        if self._buffer_size and self._write_buf:
            self.flush()
        if offset is None:
            offset = self._offset
        if offset != self._offset:
            self.seek(offset)  # To make sure we blow away our read cache
        if size == 0:  # Attempt to read entire file and return in a single return
            return self._read_file()
        else:
            rbuf = bytearray()
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
                    buf, rcode, errno = result.get()  #gevent YIELD :)
                    if rcode < 0:  # Some kind of error
                        raise IOError(errno, 'AIO Read Error %d' % errno)
                    #Rcode will be the bytes read so lets push the offset
                    self._offset = offset = offset + rcode
                    self._read_buf.extend(buf)
                    if rcode == 0 or rcode < read_size:  # Good Enough
                        self._eof = True
                #Do a buffer read
                toread = size - len(rbuf)
                rbuf.extend(memoryview(self._read_buf)[0:toread])
                #Clean up read buffer
                del self._read_buf[0:toread]
                if not self._read_buf and self._eof:  # Empty buffer and eof
                    break
            if self._eof and not rbuf:
                return None  #EOF NO DATA
            else:
                return rbuf
