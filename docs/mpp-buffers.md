# MPP buffers and codec feed structs (sole-owner harness)

This documents the concrete buffer-allocation and feed-struct ABI a sole-owner userspace process uses to drive the Artosyn Proxima-9311 codecs: allocate physical (MMZ) memory, wrap an H.265 elementary-stream chunk in a VDEC_STREAM_S and decode it, and wrap a raw NV12 frame in the VENC SendFrame descriptor and encode it. It complements `decode-display-pipeline.md` (which covers where to run and channel/pool safety) with the exact struct offsets and call signatures. Headers: `libre/sdk/include/ar_mpi_sys.h` (SYS + VB + MMZ), `ar_mpi_vdec.h`, `ar_mpi_venc.h`. Everything below is static analysis; nothing here is validated on hardware. Offsets tagged proven are byte-exact from the binaries; the rest are inferred and carry TODO(unverified).

This applies only in the sole-owner case (the vendor `ar_lowdelay` is not running, for example the open-Alpine slot). Against the running stock stack a second-process codec is unsafe; see `decode-display-pipeline.md` section 2.3.

## Two ways to get a physical buffer

The codecs DMA to and from physical addresses; userspace never mmaps codec registers, only the DMA buffers. Two allocators exist, both backed by the same MMZ carveout (the anonymous zone at phys 0x29400000 + 0x06C00000, 108 MiB).

Direct MMZ (simplest for a harness): one call returns both the physical base and a CPU pointer.

    AR_U64 phys;
    void  *virt;
    AR_MPI_SYS_MmzAlloc_Cached(&phys, &virt, "es_in", NULL, len);   // NULL zone = anonymous
    // ... write to virt ...
    AR_MPI_SYS_MmzFlushCache(phys, virt, len);                      // clean cache before device reads
    // ... hand phys (and/or virt) to the codec ...
    AR_MPI_SYS_MmzFree(phys, virt);

Signatures (proven from `ar_hal_sys_mmz_alloc` 0x5768 etc): `AR_MPI_SYS_MmzAlloc(&phys, &virt, name, zone, len)` and the `_Cached` variant take `(AR_U64 *pu64PhyAddr, void **ppVirAddr, const char *strMmb, const char *strZone, AR_U32 u32Len)` and return 0 on success. `MmzFree(phys, virt)`, `MmzFlushCache(phys, virt, size)`. To map a phys you already know, `AR_MPI_SYS_Mmap(phys, size)` (device/uncached) or `AR_MPI_SYS_MmapCache(phys, size)` (cached) return a CPU pointer; `AR_MPI_SYS_Munmap(virt, size)` releases it. Use the cached variants for CPU-heavy fill/read plus an explicit flush/invalidate; use uncached when you do not want to manage coherency.

VB pool (what the vendor pipeline uses; needed when a codec channel pulls blocks itself): create a private pool, then take blocks from it.

    AR_VB_POOL_CONFIG_S pc = {0};
    pc.u64BlkSize = ALIGN(w,16) * ALIGN(h,16) * 3 / 2;   // e.g. 1920*1088*3/2 = 0x2FD000 for NV12 1080p
    pc.u32BlkCnt  = 6;
    AR_VB_POOL pool = AR_MPI_VB_CreatePool(&pc);          // returns pool id (>=0) or -1
    AR_VB_BLK  blk  = AR_MPI_VB_GetBlock(pool, pc.u64BlkSize, NULL);
    AR_U64     blkPhys = AR_MPI_VB_Handle2PhysAddr(blk);  // MMZ phys of the block
    // map blkPhys with AR_MPI_SYS_MmapCache to get a CPU pointer for fill/read
    ...
    AR_MPI_VB_ReleaseBlock(blk);
    AR_MPI_VB_DestroyPool(pool);

Prefer `AR_MPI_VB_CreatePool` (a private pool) over `AR_MPI_VB_SetConfig` + `AR_MPI_VB_Init`, which reconfigure the GLOBAL pool layout. `VB_POOL_CONFIG_S` is 40 bytes: u64BlkSize@+0, u32BlkCnt@+8 (both proven); enRemapMode@+12 and acMmzName[24]@+16 are inferred (leave zero for the anonymous zone). `VB_CONFIG_S` (the SetConfig arg) is 648 bytes: u32MaxPoolCnt@+0, astCommPool[16]@+8 (proven base/stride). VB handles encode as (pool_id<<16)|block_index. ioctls: CreatePool `_IOW('b',4,40)`, GetBlock `_IOWR('b',6,64)`, Handle2PhysAddr `_IOWR('b',9,64)`, SetConfig `_IOW('b',0,648)`.

## Decode: feed one H.265 chunk, read one frame

Lifecycle (per `decode-display-pipeline.md`): `AR_MPI_SYS_Init()` once, then `AR_MPI_VDEC_Init(chnNum)`, `AR_MPI_VDEC_CreateChn(chn, &attr)` with `enType = AR_PT_H265`, `AR_MPI_VDEC_StartRecvStream(chn)`.

VDEC_STREAM_S (0x30 = 48 bytes; offsets proven from `AR_MPI_VDEC_SendStream` 0x9bd8). Two feed modes:

    AR_VDEC_STREAM_S s = {0};
    s.u32Len = chunkLen;                 // +0  compared vs the channel's u32StreamBufSize
    // copy mode: give a CPU pointer; the MPI memcpys it into the decoder ring + flushes cache
    s.u32VirAddrLo = (AR_U32)(uintptr_t)esVirt;   // +28 (unaligned u64 pu8Addr, exposed lo/hi)
    s.u32VirAddrHi = (AR_U32)((uint64_t)(uintptr_t)esVirt >> 32);
    // zero-copy mode instead: leave u32VirAddr* = 0 and set s.u64PhyAddr = esPhys (+8)
    AR_MPI_VDEC_SendStream(chn, &s, /*s32MilliSec*/ -1);

The +20 field is a proven u32 flag (inferred bEndOfFrame; it also gates the H26x frame-boundary parse). u64PTS / bEndOfStream / bDisplay are not touched by SendStream and their offsets are TODO(unverified); leave zero. The `pa`/`va` split is confirmed by the rodata log `invalid frame pa %#lx va %#lx`.

Read the decoded frame with `AR_MPI_VDEC_GetFrame(chn, &frameInfo, ms)` into `AR_VIDEO_FRAME_INFO_S` (the ~448-byte struct in `ar_mpi_vdec.h`): u32Width@+4, u32Height@+8, enPixelFormat@+16, plane phys u64PhyAddr0/1/2 @+144/+152/+160, strides @+356/+360/+364 (all proven in the earlier VDEC pass). Map a plane phys with `AR_MPI_SYS_MmapCache` to read the pixels. Release with `AR_MPI_VDEC_ReleaseFrame`. The decoder emits at the stream's coded resolution (it does not upscale); the ar_scaler block does display scaling.

## Encode: feed one raw NV12 frame, read the bitstream

Lifecycle: `AR_MPI_VENC_Init()`, `AR_MPI_VENC_CreateChn(chn, &attr)` (`enType`, RC mode, GOP per `venc-api.md`), `AR_MPI_VENC_StartRecvFrame(chn, &{s32RecvPicNum=-1})`.

SendFrame takes the same MPP `AR_VIDEO_FRAME_INFO_S` that GetFrame produces (feed a decoded frame straight back, or fill a fresh raw frame). Proven read offsets in the VENC packer (`AR_MPI_VENC_SendFrame` 0x8210): u32Width@+4, u32Height@+8, enPixelFormat@+16 (accepted 22/23/25/26/29/31, else I420), field/videoFormat@+24, a stride pair @+48/+52, and two u64 plane-address triplets @+72/+80/+88 and @+120/+128/+136 plus @+136 and @+240. The 3rd arg is `s32MilliSec`.

    AR_VIDEO_FRAME_INFO_S f = {0};
    f.u32Width = w; f.u32Height = h; f.enPixelFormat = 23;   // 23 = the value ar_lowdelay uses
    f.u32Stride0 = strideY; f.u32Stride1 = strideC;
    // set the plane physical addresses in the triplet(s) the encoder DMAs from:
    f.u64Addr0 = yPhys; f.u64Addr1 = uvPhys; /* f.u64Addr2 = vPhys for 3-plane */
    AR_MPI_VENC_SendFrame(chn, &f, /*s32MilliSec*/ -1);

IMPORTANT (TODO(unverified)): the VENC packer's plane/stride offsets (+72/+80/+88, +48/+52) do NOT line up with the VDEC GetFrame plane/stride offsets (+144/+152/+160, +356/+360/+364); the two RE passes agree only on +136. Which triplet is physical vs virtual, and which offsets the two paths truly share, is unresolved. For a decoded-frame-to-encoder harness, copy the frame struct GetFrame filled verbatim (the byte layout is identical on the wire). For a synthesized raw frame, populate both triplets and both stride positions with the plane phys/stride and confirm the encoder output on device before trusting a single interpretation.

Read the bitstream with `AR_MPI_VENC_GetStream(chn, &stream, block_ms)`. The caller owns the pack array:

    AR_VENC_PACK_S packs[8];
    AR_VENC_STREAM_S st = {0};
    st.pstPack = packs;         // +0  proven
    st.u32PackCount = 8;        // +8  in = capacity, out = packs returned
    AR_MPI_VENC_GetStream(chn, &st, /*block_ms*/ -1);
    for (AR_U32 i = 0; i < st.u32PackCount; i++) {
        // packs[i].pu8Addr (+8) is already a CPU pointer (MPI mmap'd packs[i].u64PhyAddr and
        // invalidated the cache); read packs[i].u32Len (+16) bytes; packs[i].u32DataType (+44).
        write(fd, (void *)(uintptr_t)packs[i].pu8Addr, packs[i].u32Len);
    }
    AR_MPI_VENC_ReleaseStream(chn, &st);

`VENC_PACK_S` is 0x98 = 152 bytes (proven stride): u64PhyAddr@+0, pu8Addr@+8 (filled by the MPI via `ar_hal_sys_mmap`/`mmap_cache`), u32Len@+16, u32DataType@+44 (all proven); u64PTS@+24 is inferred. Always `ReleaseStream` before the next `GetStream`. GetStream blocks in the codec-completion ioctl (no poll on the encoder side).

## Teardown

Reverse order, sole-owner only: `StopRecvFrame`/`StopRecvStream` -> `DestroyChn` -> `VENC_Exit`/`VDEC_DeInit` -> `ReleaseBlock`/`DestroyPool` -> free MMZ -> `AR_MPI_SYS_Exit()`. Do not call `SYS_Exit` if anything else still uses MPP.
