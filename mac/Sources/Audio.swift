import AVFoundation
import CoreMedia

/// Converts a ScreenCaptureKit audio buffer (usually planar float) to interleaved f32 samples.
func interleavedFloat32(_ sample: CMSampleBuffer) -> (rate: Int, channels: Int, samples: Data)? {
    guard let desc = CMSampleBufferGetFormatDescription(sample),
        let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(desc)?.pointee,
        asbd.mFormatID == kAudioFormatLinearPCM, asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0,
        asbd.mBitsPerChannel == 32
    else { return nil }
    let channels = Int(asbd.mChannelsPerFrame), frames = CMSampleBufferGetNumSamples(sample)
    guard channels > 0, frames > 0 else { return nil }

    var block: CMBlockBuffer?
    var listSize = 0
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sample, bufferListSizeNeededOut: &listSize, bufferListOut: nil, bufferListSize: 0,
        blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil)
    let raw = UnsafeMutableRawPointer.allocate(byteCount: listSize, alignment: MemoryLayout<AudioBufferList>.alignment)
    defer { raw.deallocate() }
    let list = raw.assumingMemoryBound(to: AudioBufferList.self)
    guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sample, bufferListSizeNeededOut: nil, bufferListOut: list, bufferListSize: listSize,
        blockBufferAllocator: nil, blockBufferMemoryAllocator: nil,
        flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, blockBufferOut: &block) == noErr
    else { return nil }

    let buffers = UnsafeMutableAudioBufferListPointer(list)
    var out = Data(count: frames * channels * 4)
    out.withUnsafeMutableBytes { dst in
        let d = dst.bindMemory(to: Float.self)
        if asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0 {
            for (c, buffer) in buffers.prefix(channels).enumerated() {
                guard let src = buffer.mData?.assumingMemoryBound(to: Float.self) else { continue }
                for i in 0..<frames { d[i * channels + c] = src[i] }
            }
        } else if let src = buffers.first?.mData {
            memcpy(dst.baseAddress!, src, min(Int(buffers[0].mDataByteSize), dst.count))
        }
    }
    return (Int(asbd.mSampleRate), channels, out)
}
