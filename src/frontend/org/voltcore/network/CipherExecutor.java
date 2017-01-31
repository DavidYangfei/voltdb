/* This file is part of VoltDB.
 * Copyright (C) 2008-2017 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

package org.voltcore.network;

import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.net.ssl.SSLEngine;

import org.voltcore.utils.CoreUtils;

import com.google_voltpatches.common.util.concurrent.ListeningExecutorService;
import com.google_voltpatches.common.util.concurrent.MoreExecutors;

import io.netty_voltpatches.buffer.ByteBuf;
import io.netty_voltpatches.buffer.PooledByteBufAllocator;

public enum CipherExecutor {

    SERVER(getWishedThreadCount()),
    CLIENT(2);

    public final static int PAGE_SHIFT = 14; // 16384 (max TLS fragment)
    public final static int PAGE_SIZE = 1 << PAGE_SHIFT;
    private final static BigInteger LSB_MASK = new BigInteger(new byte[] {
            (byte) 255,
            (byte) 255,
            (byte) 255,
            (byte) 255,
            (byte) 255,
            (byte) 255,
            (byte) 255,
            (byte) 255 });


    volatile ListeningExecutorService m_es = null;
    AtomicBoolean m_active = new AtomicBoolean(false);
    final int m_threadCount;

    private CipherExecutor(int nthreads) {
        m_threadCount = nthreads;
        m_es = null;
    }

    private static final int getWishedThreadCount() {
        Runtime rt = null;
        try {
            rt = Runtime.getRuntime();
        } catch (Throwable t) {
            rt = null;
        }
        int coreCount = rt != null ? rt.availableProcessors() : 2;
        return Math.max(2, coreCount/2);
    }

    public ListeningExecutorService getES() {
        if (m_es == null) {
            synchronized (this) {
                if (!m_active.get()) {
                    throw new IllegalStateException(name() + " cipher is not active");
                }
                if (m_es == null) {
                    ThreadFactory thrdfct = CoreUtils.getThreadFactory(
                            name () + " SSL cipher service", CoreUtils.MEDIUM_STACK_SIZE);
                    m_es = MoreExecutors.listeningDecorator(
                            Executors.newFixedThreadPool(
                                    m_threadCount,
                                    thrdfct));
                }
            }
        }
        return m_es;
    }

    public void startup() {
        if (m_active.compareAndSet(false, true)) {
            getES();
        }
    }

    public boolean isActive() {
        return m_active.get();
    }

    public void shutdown() {
        if (m_active.compareAndSet(true, false)) {
            synchronized (this) {
                m_es.shutdown();
                try {
                    m_es.awaitTermination(365, TimeUnit.DAYS);
                } catch (InterruptedException e) {
                    throw new RuntimeException(
                            "Interruped while waiting for " + name() + " cipher service shutdown",e);
                }
                m_es = null;
            }
        }
    }

    public PooledByteBufAllocator allocator() {
        switch (this) {
        case CLIENT:
            return ClientPoolHolder.INSTANCE;
        case SERVER:
            return ServerPoolHolder.INSTANCE;
        default:
            return /* impossible */ null;
        }
    }

    public final static int framesFor(int size) {
        int pages = (size >> PAGE_SHIFT);
        int modulo = size & (PAGE_SIZE - 1);
        return modulo > 0 ? pages+1 : pages;
    }

    private static class ClientPoolHolder {
        static final PooledByteBufAllocator INSTANCE =
                new PooledByteBufAllocator(
                        true,
                        PooledByteBufAllocator.defaultNumHeapArena(),
                        PooledByteBufAllocator.defaultNumDirectArena(),
                        PAGE_SIZE, /* page size */
                        PooledByteBufAllocator.defaultMaxOrder(),
                        PooledByteBufAllocator.defaultTinyCacheSize(),
                        PooledByteBufAllocator.defaultSmallCacheSize(),
                        PooledByteBufAllocator.defaultNormalCacheSize());
    }

    private static class ServerPoolHolder {
        static final PooledByteBufAllocator INSTANCE =
                new PooledByteBufAllocator(
                        true,
                        PooledByteBufAllocator.defaultNumHeapArena(),
                        PooledByteBufAllocator.defaultNumDirectArena(),
                        PAGE_SIZE, /* page size */
                        PooledByteBufAllocator.defaultMaxOrder(),
                        PooledByteBufAllocator.defaultTinyCacheSize(),
                        PooledByteBufAllocator.defaultSmallCacheSize(),
                        512);
    }

    public static CipherExecutor valueOf(SSLEngine engn) {
        return engn.getUseClientMode() ? CLIENT : SERVER;
    }

    /*
     * for debugging purposes
     */
    public static final UUID digest(ByteBuf buf, int offset) {
        if (offset < 0) return null;

        MessageDigest md = null;
        try {
            md = MessageDigest.getInstance("MD5");
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("failed to get instantiate MD5 digester", e);
        }
        md.reset();
        ByteBuf bb = buf.slice();
        if (buf.readableBytes() <= offset) {
            return null;
        }
        bb.readerIndex(bb.readerIndex() + offset);
        while (bb.isReadable()) {
            md.update(bb.readByte());
        }
        BigInteger bi = new BigInteger(1, md.digest());
        return new UUID(bi.shiftRight(64).longValue(), bi.and(LSB_MASK).longValue());
    }

    /*
     * for debugging purposes
     */
    public static final UUID digest(ByteBuffer buf, int offset) {
        if (offset < 0) return null;
        MessageDigest md = null;
        try {
            md = MessageDigest.getInstance("MD5");
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("failed to instantiate MD5 digester", e);
        }
        md.reset();
        ByteBuffer bb = null;
        if (!buf.hasRemaining() && buf.limit() > 0) {
            bb = ((ByteBuffer)buf.duplicate().flip()).asReadOnlyBuffer();
        } else {
            bb = buf.slice().asReadOnlyBuffer();
        }
        if (bb.remaining() <= offset) {
            return null;
        }
        bb.position(bb.position() + offset);
        while (bb.hasRemaining()) {
            md.update(bb.get());
        }
        BigInteger bi = new BigInteger(1, md.digest());
        return new UUID(bi.shiftRight(64).longValue(), bi.and(LSB_MASK).longValue());
    }
}
