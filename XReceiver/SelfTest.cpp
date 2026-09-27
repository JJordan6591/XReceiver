#include "pch.h"
#include "SelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "AudioPlayout.h"
#include "AudioReceiver.h"
#include "DebugLog.h"
#include "H264Bitstream.h"
#include "H264Depacketizer.h"
#include "H264KeyframeGate.h"
#include "JitterBuffer.h"
#include "L16Convert.h"
#include "LifecycleGuard.h"
#include "MediaClock.h"
#include "PcmRingBuffer.h"
#include "RtpPacket.h"
#include "RtpSequence.h"
#include "SincResampler.h"
#include "FrameDelivery.h"
#include "VideoDeliveryCore.h"
#include "VideoTimeline.h"
#include "ReceiverSettings.h"
#include "Health.h"
#include "UiChrome.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;

namespace rx
{
    namespace
    {
        struct TestContext
        {
            int passed = 0;
            int failed = 0;
            std::wstring failures;

            void Check(bool condition, wchar_t const* name)
            {
                if (condition)
                {
                    ++passed;
                    return;
                }
                ++failed;
                Log(L"SELFTEST FAIL: %s", name);
                if (failed <= 8)
                {
                    failures += L"\n  FAIL: ";
                    failures += name;
                }
            }
        };

        using Bytes = std::vector<uint8_t>;

        Bytes MakeRtp(uint16_t sequence, uint32_t timestamp, uint32_t ssrc, bool marker, Bytes const& payload,
                      uint8_t payloadType = 96)
        {
            Bytes packet(12);
            packet[0] = 0x80;
            packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | payloadType);
            packet[2] = static_cast<uint8_t>(sequence >> 8);
            packet[3] = static_cast<uint8_t>(sequence);
            packet[4] = static_cast<uint8_t>(timestamp >> 24);
            packet[5] = static_cast<uint8_t>(timestamp >> 16);
            packet[6] = static_cast<uint8_t>(timestamp >> 8);
            packet[7] = static_cast<uint8_t>(timestamp);
            packet[8] = static_cast<uint8_t>(ssrc >> 24);
            packet[9] = static_cast<uint8_t>(ssrc >> 16);
            packet[10] = static_cast<uint8_t>(ssrc >> 8);
            packet[11] = static_cast<uint8_t>(ssrc);
            packet.insert(packet.end(), payload.begin(), payload.end());
            return packet;
        }

        class BitWriter
        {
        public:
            void Bits(uint32_t value, int count)
            {
                for (int i = count - 1; i >= 0; --i)
                {
                    Bit((value >> i) & 1u);
                }
            }
            void Bit(uint32_t bit)
            {
                if (m_bit == 0)
                {
                    m_bytes.push_back(0);
                }
                if (bit)
                {
                    m_bytes.back() |= static_cast<uint8_t>(0x80 >> m_bit);
                }
                m_bit = (m_bit + 1) % 8;
            }
            void Ue(uint32_t value)
            {
                uint64_t const v = uint64_t{ value } + 1;
                int bits = 0;
                while ((v >> bits) > 1)
                {
                    ++bits;
                }
                Bits(0, bits);
                Bits(static_cast<uint32_t>(v), bits + 1);
            }
            Bytes Finish()
            {
                Bit(1); // rbsp_stop_one_bit
                while (m_bit != 0)
                {
                    Bit(0);
                }
                return m_bytes;
            }

        private:
            Bytes m_bytes;
            int m_bit = 0;
        };

        Bytes AddEmulationPrevention(Bytes const& rbsp)
        {
            Bytes out;
            int zeros = 0;
            for (uint8_t b : rbsp)
            {
                if (zeros >= 2 && b <= 3)
                {
                    out.push_back(3);
                    zeros = 0;
                }
                out.push_back(b);
                zeros = (b == 0) ? zeros + 1 : 0;
            }
            return out;
        }

        Bytes MakeSps(bool highProfile, uint32_t widthMbs, uint32_t heightMbs, uint32_t cropBottom, bool withRestriction,
                      uint32_t level = 40)
        {
            BitWriter w;
            w.Bits(highProfile ? 100 : 66, 8);
            w.Bits(0, 8);
            w.Bits(level, 8);
            w.Ue(0); // sps id
            if (highProfile)
            {
                w.Ue(1); // chroma_format_idc
                w.Ue(0);
                w.Ue(0);
                w.Bit(0);
                w.Bit(0); // no scaling matrix
            }
            w.Ue(0); // log2_max_frame_num_minus4
            w.Ue(0); // poc type 0
            w.Ue(0); // log2_max_poc_lsb_minus4
            w.Ue(1); // max_num_ref_frames
            w.Bit(0);
            w.Ue(widthMbs - 1);
            w.Ue(heightMbs - 1);
            w.Bit(1); // frame_mbs_only
            w.Bit(1); // direct_8x8
            if (cropBottom > 0)
            {
                w.Bit(1);
                w.Ue(0);
                w.Ue(0);
                w.Ue(0);
                w.Ue(cropBottom);
            }
            else
            {
                w.Bit(0);
            }
            if (withRestriction)
            {
                w.Bit(1); // vui present
                w.Bit(0); // aspect
                w.Bit(0); // overscan
                w.Bit(0); // video signal
                w.Bit(0); // chroma loc
                w.Bit(1); // timing info
                w.Bits(1, 32);
                w.Bits(120, 32);
                w.Bit(1);
                w.Bit(0); // nal hrd
                w.Bit(0); // vcl hrd
                w.Bit(0); // pic struct
                w.Bit(1); // bitstream restriction
                w.Bit(1);
                w.Ue(2);
                w.Ue(1);
                w.Ue(16);
                w.Ue(16);
                w.Ue(0); // num_reorder_frames
                w.Ue(1); // max_dec_frame_buffering
            }
            else
            {
                w.Bit(0);
            }
            Bytes nal{ 0x67 };
            Bytes const body = AddEmulationPrevention(w.Finish());
            nal.insert(nal.end(), body.begin(), body.end());
            return nal;
        }

        Bytes const kPps{ 0x68, 0xCE, 0x3C, 0x80 };
        Bytes const kIdrSlice{ 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF };
        Bytes const kPSlice{ 0x41, 0x9A, 0x02, 0x04 };
        Bytes const kNonRefSlice{ 0x01, 0x9E, 0x02, 0x04 };

        struct CollectingSink : JitterBuffer::Sink
        {
            std::vector<int64_t> delivered;
            int64_t lost = 0;
            void OnOrderedPacket(RtpPacketView const&, int64_t extSequence, Clock::time_point) override
            {
                delivered.push_back(extSequence);
            }
            void OnPacketsLost(int64_t count) override { lost += count; }
        };

        void TestRtpParser(TestContext& t)
        {
            RtpPacketView view;
            Bytes basic = MakeRtp(0x1234, 0xAABBCCDD, 0x01020304, true, { 1, 2, 3 });
            t.Check(ParseRtpPacket(basic.data(), basic.size(), view) == RtpParseResult::Ok, L"rtp basic parse");
            t.Check(view.sequence == 0x1234 && view.timestamp == 0xAABBCCDD && view.ssrc == 0x01020304, L"rtp header fields");
            t.Check(view.marker && view.payloadType == 96 && view.payloadSize == 3 && view.payload[0] == 1, L"rtp marker/pt/payload");

            Bytes shortPacket(11, 0x80);
            t.Check(ParseRtpPacket(shortPacket.data(), shortPacket.size(), view) == RtpParseResult::TooShort, L"rtp too short");

            Bytes badVersion = basic;
            badVersion[0] = 0x40;
            t.Check(ParseRtpPacket(badVersion.data(), badVersion.size(), view) == RtpParseResult::BadVersion, L"rtp bad version");

            Bytes padded = MakeRtp(1, 0, 1, false, { 9, 9, 0, 0, 3 });
            padded[0] |= 0x20;
            t.Check(ParseRtpPacket(padded.data(), padded.size(), view) == RtpParseResult::Ok && view.payloadSize == 2, L"rtp padding");

            Bytes badPadding = MakeRtp(1, 0, 1, false, { 9, 9, 7 });
            badPadding[0] |= 0x20;
            t.Check(ParseRtpPacket(badPadding.data(), badPadding.size(), view) == RtpParseResult::BadPadding, L"rtp bad padding");

            Bytes csrc = MakeRtp(1, 0, 1, false, { 0, 0, 0, 5, 0xEE });
            csrc[0] |= 0x01;
            t.Check(ParseRtpPacket(csrc.data(), csrc.size(), view) == RtpParseResult::Ok && view.payloadSize == 1 && view.payload[0] == 0xEE, L"rtp csrc skip");

            Bytes badCsrc = MakeRtp(1, 0, 1, false, { 0 });
            badCsrc[0] |= 0x0F;
            t.Check(ParseRtpPacket(badCsrc.data(), badCsrc.size(), view) == RtpParseResult::BadCsrc, L"rtp bad csrc");

            Bytes extension = MakeRtp(1, 0, 1, false, { 0xBE, 0xDE, 0, 1, 1, 2, 3, 4, 0x77 });
            extension[0] |= 0x10;
            t.Check(ParseRtpPacket(extension.data(), extension.size(), view) == RtpParseResult::Ok && view.payloadSize == 1 && view.payload[0] == 0x77, L"rtp extension skip");

            Bytes badExtension = MakeRtp(1, 0, 1, false, { 0xBE, 0xDE, 0, 9, 1 });
            badExtension[0] |= 0x10;
            t.Check(ParseRtpPacket(badExtension.data(), badExtension.size(), view) == RtpParseResult::BadExtension, L"rtp bad extension");

            Bytes empty = MakeRtp(1, 0, 1, false, {});
            t.Check(ParseRtpPacket(empty.data(), empty.size(), view) == RtpParseResult::EmptyPayload, L"rtp empty payload");
        }

        void TestUnwrappers(TestContext& t)
        {
            SequenceUnwrapper seq;
            int64_t const a = seq.Unwrap(65534);
            int64_t const b = seq.Unwrap(65535);
            int64_t const c = seq.Unwrap(0);
            int64_t const d = seq.Unwrap(1);
            t.Check(b == a + 1 && c == a + 2 && d == a + 3, L"sequence wrap 65535->0");
            t.Check(seq.Unwrap(65535) == b, L"sequence reordered across wrap");

            TimestampUnwrapper ts;
            int64_t const t0 = ts.Unwrap(0xFFFFFF00u);
            int64_t const t1 = ts.Unwrap(0x00000100u);
            t.Check(t1 - t0 == 0x200, L"timestamp wrap");
        }

        void TestJitterBuffer(TestContext& t)
        {
            JitterBuffer::Config config;
            config.reorderWindow = 16;
            config.capacity = 32;
            config.timeout = std::chrono::milliseconds(10);
            JitterBuffer jitter(config);
            CollectingSink sink;
            SequenceUnwrapper seq;
            auto now = Clock::now();

            auto insert = [&](uint16_t s, Clock::time_point at)
            {
                Bytes packet = MakeRtp(s, 0, 1, false, { 1 });
                RtpPacketView view;
                ParseRtpPacket(packet.data(), packet.size(), view);
                return jitter.Insert(packet.data(), packet.size(), view, seq.Unwrap(s), at, sink);
            };

            insert(65533, now);
            insert(65534, now);
            auto const buffered = insert(0, now);         // 65535 missing
            auto const filled = insert(65535, now);
            t.Check(buffered == JitterBuffer::InsertResult::Buffered, L"jitter buffers ahead-of-sequence packet");
            t.Check(filled == JitterBuffer::InsertResult::DeliveredReordered, L"jitter reports reordered fill");
            t.Check(sink.delivered.size() == 4 && sink.delivered[2] + 1 == sink.delivered[3], L"jitter delivers in order across wrap");
            t.Check(insert(65535, now) == JitterBuffer::InsertResult::Duplicate, L"jitter duplicate");

            insert(3, now);                                 // 1 and 2 missing
            jitter.Poll(now + std::chrono::milliseconds(5), sink);
            t.Check(sink.lost == 0, L"jitter holds gap within timeout");
            jitter.Poll(now + std::chrono::milliseconds(11), sink);
            t.Check(sink.lost == 2 && sink.delivered.back() == seq.Unwrap(3), L"jitter releases gap after timeout");
            t.Check(insert(1, now) == JitterBuffer::InsertResult::Late, L"jitter late packet after skip");

            size_t const before = sink.delivered.size();
            t.Check(insert(200, now) == JitterBuffer::InsertResult::Resynced && sink.delivered.size() == before + 1, L"jitter resync beyond window");
        }

        void TestTracker(TestContext& t)
        {
            RtpStreamTracker tracker;
            auto const now = Clock::now();
            auto const takeover = std::chrono::seconds(1);
            RtpPacketView p;
            p.ssrc = 0xA;
            p.sequence = 100;
            t.Check(tracker.Check(p, now, takeover) == TrackDecision::NewStream, L"tracker locks first stream");
            p.sequence = 101;
            t.Check(tracker.Check(p, now, takeover) == TrackDecision::Accept, L"tracker accepts locked stream");

            RtpPacketView other = p;
            other.ssrc = 0xB;
            other.sequence = 5000;
            t.Check(tracker.Check(other, now, takeover) == TrackDecision::Ignore, L"tracker ignores single foreign packet");
            other.sequence = 5001;
            t.Check(tracker.Check(other, now, takeover) == TrackDecision::NewStream, L"tracker switches after probation");

            RtpPacketView jump = other;
            jump.sequence = 40000;
            t.Check(tracker.Check(jump, now, takeover) == TrackDecision::Ignore, L"tracker ignores one sequence jump");
            jump.sequence = 40001;
            t.Check(tracker.Check(jump, now, takeover) == TrackDecision::NewStream, L"tracker restarts on confirmed jump");

            RtpPacketView late = jump;
            late.ssrc = 0xC;
            t.Check(tracker.Check(late, now + std::chrono::seconds(2), takeover) == TrackDecision::NewStream, L"tracker takeover after silence");
        }

        void TestRtpAdmission(TestContext& t)
        {
            RtpStreamTracker tracker;
            RtpAdmissionRules rules;
            rules.payloadType = 96;
            rules.maxPacketBytes = 2048;
            rules.minPayloadBytes = 4;
            rules.takeoverAfterSilence = std::chrono::seconds(1);
            auto const now = Clock::now();
            RtpPacketView view;
            auto admit = [&](Bytes const& datagram) { return AdmitRtpPacket(datagram.data(), datagram.size(), rules, tracker, now, view); };
            Bytes const media(8, 0x11);

            Bytes const garbage{ 0x12, 0x34, 0x56 };
            Bytes const wrongPt = MakeRtp(1, 0, 0xA, false, media, 97);
            Bytes const oversize = MakeRtp(1, 0, 0xA, false, Bytes(3000, 0x11));
            Bytes const tooShort = MakeRtp(1, 0, 0xA, false, { 0x11, 0x22 });
            t.Check(admit(garbage) == RtpAdmission::Invalid && admit(wrongPt) == RtpAdmission::WrongPayloadType &&
                    admit(oversize) == RtpAdmission::TooLarge && admit(tooShort) == RtpAdmission::TooShort && !tracker.Locked(),
                    L"admission rejects malformed, mistyped and mis-sized packets before the tracker");

            t.Check(admit(MakeRtp(10, 0, 0xA, false, media)) == RtpAdmission::NewStream && tracker.Ssrc() == 0xA &&
                    admit(MakeRtp(11, 400, 0xA, false, media)) == RtpAdmission::Accept, L"admission locks and accepts a valid stream");

            bool const oversizeNoTakeover = admit(MakeRtp(500, 0, 0xB, false, Bytes(3000, 0x11))) == RtpAdmission::TooLarge &&
                                            admit(MakeRtp(501, 0, 0xB, false, Bytes(3000, 0x11))) == RtpAdmission::TooLarge;
            bool const wrongPtNoTakeover = admit(MakeRtp(600, 0, 0xB, false, media, 97)) == RtpAdmission::WrongPayloadType &&
                                           admit(MakeRtp(601, 0, 0xB, false, media, 97)) == RtpAdmission::WrongPayloadType;
            t.Check(oversizeNoTakeover && wrongPtNoTakeover && tracker.Ssrc() == 0xA &&
                    admit(MakeRtp(12, 800, 0xA, false, media)) == RtpAdmission::Accept,
                    L"admission: rejected foreign packets never count toward takeover");

            t.Check(admit(MakeRtp(700, 0, 0xC, false, media)) == RtpAdmission::Ignored &&
                    admit(MakeRtp(701, 0, 0xC, false, media)) == RtpAdmission::NewStream && tracker.Ssrc() == 0xC,
                    L"admission keeps probation takeover for a valid new sender");
        }

        Bytes StapA(std::vector<Bytes> const& nals)
        {
            Bytes stap{ 0x78 };
            for (Bytes const& nal : nals)
            {
                stap.push_back(static_cast<uint8_t>(nal.size() >> 8));
                stap.push_back(static_cast<uint8_t>(nal.size()));
                stap.insert(stap.end(), nal.begin(), nal.end());
            }
            return stap;
        }

        Bytes FuA(uint8_t nalHeader, bool start, bool end, Bytes const& fragment)
        {
            Bytes packet{ static_cast<uint8_t>((nalHeader & 0xE0) | h264::kNalFuA),
                          static_cast<uint8_t>((start ? 0x80 : 0) | (end ? 0x40 : 0) | (nalHeader & 0x1F)) };
            packet.insert(packet.end(), fragment.begin(), fragment.end());
            return packet;
        }

        // Splits a NAL into FU-A packets carrying at most chunk payload bytes each.
        std::vector<Bytes> Fragment(Bytes const& nal, size_t chunk)
        {
            std::vector<Bytes> packets;
            for (size_t offset = 1; offset < nal.size(); offset += chunk)
            {
                size_t const n = std::min(chunk, nal.size() - offset);
                Bytes const piece(nal.begin() + static_cast<ptrdiff_t>(offset), nal.begin() + static_cast<ptrdiff_t>(offset + n));
                packets.push_back(FuA(nal[0], offset == 1, offset + n == nal.size(), piece));
            }
            return packets;
        }

        Bytes const kIdrSliceLong{ 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0x10, 0x20 };
        Bytes const kPSliceLong{ 0x41, 0x9A, 0x02, 0x04, 0x11, 0x22 };
        Bytes const kNonRefSliceLong{ 0x01, 0x9E, 0x02, 0x04, 0x11, 0x22 };
        Bytes const kPSliceNotMb0{ 0x41, 0x40, 0x02, 0x04 };   // first_mb_in_slice == 1

        class DepackHarness
        {
        public:
            DepackHarness() : depack([this](AccessUnitPtr au) { units.push_back(std::move(au)); }) {}
            DepackHarness(DepackHarness const&) = delete;
            DepackHarness& operator=(DepackHarness const&) = delete;

            void Send(int64_t sequence, int64_t timestamp, bool marker, Bytes const& payload, uint32_t ssrc = 0x1234)
            {
                H264Depacketizer::PacketInfo info;
                info.ssrc = ssrc;
                info.extSequence = sequence;
                info.extTimestamp = timestamp;
                info.marker = marker;
                info.arrival = Clock::now();
                depack.OnPacket(payload.data(), payload.size(), info);
            }

            // Sends a NAL as FU-A fragments starting at sequence, skipping the given indices.
            int64_t SendFragmented(int64_t sequence, int64_t timestamp, Bytes const& nal, size_t chunk,
                                   std::vector<size_t> const& skip = {}, bool marker = true)
            {
                auto const packets = Fragment(nal, chunk);
                for (size_t i = 0; i < packets.size(); ++i)
                {
                    bool const skipped = std::find(skip.begin(), skip.end(), i) != skip.end();
                    if (!skipped)
                    {
                        Send(sequence, timestamp, marker && i + 1 == packets.size(), packets[i]);
                    }
                    ++sequence;
                }
                return sequence;
            }

            // Pattern of emitted AUs: 'C' complete, 'I' incomplete.
            std::wstring Pattern() const
            {
                std::wstring pattern;
                for (auto const& au : units)
                {
                    pattern += au->corrupt ? L'I' : L'C';
                }
                return pattern;
            }

            bool IncompleteCarryNoData() const
            {
                for (auto const& au : units)
                {
                    if (au->corrupt && (!au->data.empty() || !au->nals.empty() || au->hasSlice))
                    {
                        return false;
                    }
                }
                return true;
            }

            std::vector<AccessUnitPtr> units;
            H264Depacketizer depack;
        };

        Bytes NalBytes(AccessUnit const& au, size_t index)
        {
            NalRef const& ref = au.nals[index];
            return Bytes(au.data.begin() + ref.offset, au.data.begin() + ref.offset + ref.size);
        }

        void TestDepacketizer(TestContext& t)
        {
            Bytes const sps = MakeSps(false, 120, 68, 4, false);
            bool noData = true;

            {
                // STAP-A (AUD, SPS, PPS) then an IDR in three FU-A fragments.
                DepackHarness h;
                h.Send(100, 1000, false, StapA({ { 0x09, 0xF0 }, sps, kPps }));
                h.SendFragmented(101, 1000, kIdrSliceLong, 3);
                t.Check(h.Pattern() == L"C", L"depack complete IDR on marker");
                if (h.units.size() == 1)
                {
                    AccessUnit const& au = *h.units[0];
                    t.Check(au.isIdr && au.hasSps && au.hasPps && au.hasSlice && au.nals.size() == 3, L"depack IDR flags, AUD stripped");
                    t.Check(au.nals.size() == 3 && NalBytes(au, 2) == kIdrSliceLong, L"depack FU-A rebuilds original IDR NAL");
                    t.Check(au.firstSequence == 100 && au.lastSequence == 103 && au.ssrc == 0x1234, L"depack AU sequence span and SSRC");
                }
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // NRI and type are rebuilt from the FU indicator and FU header.
                DepackHarness h;
                h.SendFragmented(1, 3000, kPSliceLong, 2);
                h.SendFragmented(4, 6000, kNonRefSliceLong, 2);
                t.Check(h.Pattern() == L"CC" && NalBytes(*h.units[0], 0) == kPSliceLong && NalBytes(*h.units[1], 0) == kNonRefSliceLong,
                        L"depack FU-A rebuilds NRI 2 and NRI 0 headers");
                t.Check(h.units.size() == 2 && !h.units[0]->allNonRef && h.units[1]->allNonRef, L"depack reference flags");
            }
            {
                // FU-A start fragment lost.
                DepackHarness h;
                h.Send(1, 1000, true, kPSlice);
                h.SendFragmented(2, 2500, kIdrSliceLong, 3, { 0 });
                t.Check(h.Pattern() == L"CI" && !h.units[1]->lossConfined, L"depack FU-A loss at start");
                t.Check(h.depack.GetCounters().sequenceGaps == 1 && h.depack.GetCounters().missingPackets == 1, L"depack counts gap");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // FU-A middle fragment lost.
                DepackHarness h;
                h.SendFragmented(10, 1000, kIdrSliceLong, 2, { 1 });
                t.Check(h.Pattern() == L"I" && h.units[0]->lossConfined && h.units[0]->sawSlice && !h.units[0]->allNonRef,
                        L"depack FU-A loss in middle");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // FU-A end fragment (the marker packet) lost: both neighbours are suspect.
                DepackHarness h;
                int64_t const next = h.SendFragmented(20, 1000, kIdrSliceLong, 3, { 2 });
                h.Send(next, 2500, true, kPSlice);
                h.Send(next + 1, 4000, true, kPSlice);
                t.Check(h.Pattern() == L"IIC", L"depack FU-A loss at end");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // Marker on a middle fragment: FU never ended.
                DepackHarness h;
                auto const packets = Fragment(kIdrSliceLong, 3);
                h.Send(30, 1000, false, packets[0]);
                h.Send(31, 1000, true, packets[1]);
                t.Check(h.Pattern() == L"I" && h.depack.GetCounters().fuaErrors == 1, L"depack marker without FU-A end");
            }
            {
                // Start and end bits both set, fragment type change, forbidden bit, invalid type.
                DepackHarness h;
                h.Send(40, 1000, true, FuA(0x65, true, true, { 0x88 }));
                auto const packets = Fragment(kIdrSliceLong, 3);
                h.Send(41, 2000, false, packets[0]);
                Bytes retyped = packets[2];
                retyped[1] = static_cast<uint8_t>((retyped[1] & 0xE0) | h264::kNalSliceNonIdr);
                h.Send(42, 2000, true, retyped);
                Bytes forbidden = FuA(0x65, true, false, { 0x88 });
                forbidden[0] |= 0x80;
                h.Send(43, 3000, true, forbidden);
                h.Send(44, 4000, true, FuA(0x7C, true, false, { 0x88 }));   // FU-A carrying type 28
                t.Check(h.Pattern() == L"IIII", L"depack rejects malformed FU-A headers");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // Timestamp change before the marker, with no sequence gap.
                DepackHarness h;
                h.Send(50, 4000, false, kPSlice);
                h.Send(51, 5500, true, kPSlice);
                t.Check(h.Pattern() == L"IC" && h.depack.GetCounters().missingMarker == 1, L"depack timestamp change before marker");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // Marker packet lost: the next AU may have lost its head too.
                DepackHarness h;
                h.Send(60, 6000, false, kPSlice);
                h.Send(62, 7500, true, kPSlice);
                h.Send(63, 9000, true, kPSlice);
                t.Check(h.Pattern() == L"IIC" && !h.units[0]->lossConfined && !h.units[1]->lossConfined, L"depack marker loss");
            }
            {
                // SSRC change inside an AU.
                DepackHarness h;
                h.Send(70, 1000, false, kPSlice, 0xA);
                h.Send(71, 1000, true, kPSlice, 0xB);
                t.Check(h.units.size() == 2 && h.units[0]->corrupt && h.units[0]->ssrc == 0xA && h.units[1]->ssrc == 0xB,
                        L"depack groups by SSRC");
            }
            {
                // Duplicates and stale packets never reopen or extend an AU.
                DepackHarness h;
                auto const packets = Fragment(kIdrSliceLong, 3);
                h.Send(80, 1000, false, packets[0]);
                h.Send(80, 1000, false, packets[0]);
                h.Send(81, 1000, false, packets[1]);
                h.Send(82, 1000, true, packets[2]);
                h.Send(81, 1000, false, packets[1]);
                t.Check(h.Pattern() == L"C" && NalBytes(*h.units[0], 0) == kIdrSliceLong && h.depack.GetCounters().staleOrDuplicate == 2,
                        L"depack ignores duplicate and stale packets");
            }
            {
                // Malformed STAP-A: length overflow, nested FU-A type, header only.
                DepackHarness h;
                h.Send(90, 1000, true, Bytes{ 0x78, 0x00, 0x10, 0x67 });
                h.Send(91, 2000, true, StapA({ { 0x7C, 0x85, 0x88 } }));
                h.Send(92, 3000, true, Bytes{ 0x78 });
                t.Check(h.Pattern() == L"III" && h.depack.GetCounters().stapaErrors == 3, L"depack rejects malformed STAP-A");
                noData = noData && h.IncompleteCarryNoData();
            }
            {
                // Unsupported packetization (FU-B) loses content.
                DepackHarness h;
                h.Send(100, 1000, true, Bytes{ 0x7D, 0x85, 0x88, 0x00, 0x00 });
                t.Check(h.Pattern() == L"I" && h.depack.GetCounters().unsupportedNal == 1, L"depack unsupported NAL type is incomplete");
            }
            {
                // Joining mid-picture after a reset: the first slice does not start at MB 0.
                DepackHarness h;
                h.Send(110, 1000, true, kPSliceNotMb0);
                h.Send(111, 2500, true, kPSliceNotMb0);
                t.Check(h.Pattern() == L"IC", L"depack rejects first AU that joins mid-picture");
                h.depack.Reset();
                h.units.clear();
                h.Send(500, 9000, true, kPSlice);
                t.Check(h.Pattern() == L"C", L"depack accepts first AU starting at MB 0");
            }
            {
                // Loss confined to a non-reference picture.
                DepackHarness h;
                h.SendFragmented(120, 1000, kNonRefSliceLong, 2, { 1 });
                t.Check(h.Pattern() == L"I" && h.units[0]->lossConfined && h.units[0]->sawSlice && h.units[0]->allNonRef,
                        L"depack non-reference loss is confined");
            }

            t.Check(noData, L"depack incomplete AUs never carry data");
        }

        // Jitter buffer + unwrappers + depacketizer, as wired in VideoReceiver.
        class PipelineHarness : private JitterBuffer::Sink
        {
        public:
            explicit PipelineHarness(JitterBuffer::Config const& config) : jitter(config) {}

            JitterBuffer::InsertResult Feed(uint16_t sequence, uint32_t timestamp, bool marker, Bytes const& payload,
                                            Clock::time_point now)
            {
                Bytes const packet = MakeRtp(sequence, timestamp, 0x77, marker, payload);
                RtpPacketView view;
                ParseRtpPacket(packet.data(), packet.size(), view);
                return jitter.Insert(packet.data(), packet.size(), view, sequences.Unwrap(view.sequence), now, *this);
            }

            void Poll(Clock::time_point now) { jitter.Poll(now, *this); }

            DepackHarness out;
            JitterBuffer jitter;
            SequenceUnwrapper sequences;
            TimestampUnwrapper timestamps;
            int64_t lost = 0;

        private:
            void OnOrderedPacket(RtpPacketView const& packet, int64_t extSequence, Clock::time_point arrival) override
            {
                H264Depacketizer::PacketInfo info;
                info.ssrc = packet.ssrc;
                info.extSequence = extSequence;
                info.extTimestamp = timestamps.Unwrap(packet.timestamp);
                info.marker = packet.marker;
                info.arrival = arrival;
                out.depack.OnPacket(packet.payload, packet.payloadSize, info);
            }
            void OnPacketsLost(int64_t count) override { lost += count; }
        };

        void TestVideoPipeline(TestContext& t)
        {
            JitterBuffer::Config config;
            config.reorderWindow = 64;
            config.capacity = 128;
            config.timeout = std::chrono::milliseconds(10);
            auto const now = Clock::now();
            auto const frags = Fragment(kIdrSliceLong, 3);

            {
                // Sequence and timestamp wrap inside and across access units.
                PipelineHarness p(config);
                uint32_t const ts0 = 0xFFFFFA00u;
                p.Feed(65533, ts0, false, frags[0], now);
                p.Feed(65534, ts0, false, frags[1], now);
                p.Feed(65535, ts0, true, frags[2], now);
                p.Feed(0, ts0 + 1500u, true, kPSlice, now);
                p.Feed(1, ts0 + 3000u, true, kPSlice, now);
                auto const& units = p.out.units;
                t.Check(p.out.Pattern() == L"CCC" && p.lost == 0 && p.out.depack.GetCounters().sequenceGaps == 0,
                        L"pipeline sequence wrap without loss");
                t.Check(units.size() == 3 && units[2]->rtpTimestamp - units[1]->rtpTimestamp == 1500 &&
                        units[1]->rtpTimestamp - units[0]->rtpTimestamp == 1500, L"pipeline timestamp wrap");
            }
            {
                // A fragment lost across the sequence wrap.
                PipelineHarness p(config);
                p.Feed(65534, 1000, false, frags[0], now);
                p.Feed(0, 1000, true, frags[2], now);
                p.Poll(now + std::chrono::milliseconds(11));
                t.Check(p.out.Pattern() == L"I" && p.lost == 1, L"pipeline loss across sequence wrap");
            }
            {
                // Out-of-order fragments are reordered into a complete AU.
                PipelineHarness p(config);
                p.Feed(100, 1000, false, frags[0], now);
                p.Feed(102, 1000, true, frags[2], now);
                auto const fill = p.Feed(101, 1000, false, frags[1], now);
                t.Check(fill == JitterBuffer::InsertResult::DeliveredReordered && p.out.Pattern() == L"C" &&
                        NalBytes(*p.out.units[0], 0) == kIdrSliceLong, L"pipeline reorders out-of-order fragments");
            }
            {
                // Duplicate fragments are discarded.
                PipelineHarness p(config);
                p.Feed(200, 1000, false, frags[0], now);
                auto const dup = p.Feed(200, 1000, false, frags[0], now);
                p.Feed(201, 1000, false, frags[1], now);
                p.Feed(202, 1000, true, frags[2], now);
                t.Check(dup == JitterBuffer::InsertResult::Duplicate && p.out.Pattern() == L"C" &&
                        NalBytes(*p.out.units[0], 0) == kIdrSliceLong, L"pipeline drops duplicate fragment");
            }
            {
                // A fragment arriving after the reorder timeout is late; its AU stays discarded.
                PipelineHarness p(config);
                p.Feed(300, 1000, false, frags[0], now);
                p.Feed(302, 1000, true, frags[2], now);
                p.Poll(now + std::chrono::milliseconds(11));
                auto const late = p.Feed(301, 1000, false, frags[1], now + std::chrono::milliseconds(12));
                p.Feed(303, 2500, true, kPSlice, now + std::chrono::milliseconds(13));
                t.Check(late == JitterBuffer::InsertResult::Late && p.out.Pattern() == L"IC", L"pipeline late packet after timeout");
                t.Check(p.out.IncompleteCarryNoData(), L"pipeline incomplete AUs carry no data");
            }
        }

        void TestSps(TestContext& t)
        {
            SpsInfo info;
            Bytes const baseline = MakeSps(false, 120, 68, 4, false);
            t.Check(ParseSps(baseline.data(), baseline.size(), info) && info.width == 1920 && info.height == 1080, L"sps baseline 1080p crop");

            Bytes const high = MakeSps(true, 80, 45, 0, true);
            bool const ok = ParseSps(high.data(), high.size(), info);
            t.Check(ok && info.profileIdc == 100 && info.width == 1280 && info.height == 720, L"sps high 720p");
            t.Check(ok && info.bitstreamRestriction && info.maxDecFrameBuffering == 1 && info.numReorderFrames == 0, L"sps vui bitstream restriction");
            t.Check(ok && info.timingInfoPresent && info.timeScale == 120, L"sps vui timing");

            Bytes truncated(baseline.begin(), baseline.begin() + 4);
            t.Check(!ParseSps(truncated.data(), truncated.size(), info), L"sps rejects truncated");

            Bytes const hevcVps{ 0x40, 0x01, 0x0C };
            t.Check(LooksLikeHevcPayload(hevcVps.data(), hevcVps.size()), L"hevc heuristic VPS");
            t.Check(!LooksLikeHevcPayload(kIdrSlice.data(), kIdrSlice.size()) && !LooksLikeHevcPayload(kPSlice.data(), kPSlice.size()), L"hevc heuristic ignores h264");
        }

        AccessUnitPtr MakeAu(std::vector<Bytes> const& nals, bool corrupt = false)
        {
            auto au = std::make_unique<AccessUnit>();
            for (Bytes const& nal : nals)
            {
                au->data.insert(au->data.end(), std::begin(h264::kStartCode), std::end(h264::kStartCode));
                NalRef ref;
                ref.offset = static_cast<uint32_t>(au->data.size());
                ref.size = static_cast<uint32_t>(nal.size());
                ref.type = nal[0] & 0x1F;
                au->data.insert(au->data.end(), nal.begin(), nal.end());
                au->nals.push_back(ref);
                uint8_t const nri = (nal[0] >> 5) & 3;
                if (ref.type >= 1 && ref.type <= 5)
                {
                    au->hasSlice = true;
                    au->allNonRef = au->allNonRef && nri == 0;
                    au->isIdr = au->isIdr || ref.type == 5;
                }
                au->hasSps = au->hasSps || ref.type == 7;
                au->hasPps = au->hasPps || ref.type == 8;
            }
            au->sawSlice = au->hasSlice;
            if (corrupt)
            {
                // Mirrors the depacketizer: damaged AUs keep classification but no data.
                au->corrupt = true;
                au->data.clear();
                au->nals.clear();
                au->hasSlice = false;
                au->hasSps = false;
                au->hasPps = false;
            }
            return au;
        }

        using Decision = H264KeyframeGate::Decision;

        void TestGate(TestContext& t)
        {
            H264KeyframeGate gate;
            H264KeyframeGate::Config config;
            config.policy = LossPolicy::Strict;
            gate.Configure(config);
            gate.Reset();
            Bytes const sps = MakeSps(false, 120, 68, 4, false);

            t.Check(gate.Process(*MakeAu({ kPSlice })).decision == Decision::DropAwaitingIdr, L"gate drops P before first IDR");
            t.Check(gate.Process(*MakeAu({ kIdrSlice })).decision == Decision::DropAwaitingIdr, L"gate needs parameter sets for IDR");

            auto idr = MakeAu({ sps, kPps, kIdrSlice });
            auto result = gate.Process(*idr);
            t.Check(result.decision == Decision::Submit && result.resynced && idr->discontinuity && !gate.IsWaiting(),
                    L"gate submits IDR with SPS/PPS");
            t.Check(gate.Process(*MakeAu({ kPSlice })).decision == Decision::Submit, L"gate submits P while decoding");

            auto nonRef = MakeAu({ kNonRefSlice }, true);
            t.Check(gate.Process(*nonRef).decision == Decision::DropNonRef && !gate.IsWaiting(), L"gate drops confined non-ref loss without waiting");

            auto straddling = MakeAu({ kNonRefSlice }, true);
            straddling->lossConfined = false;
            result = gate.Process(*straddling);
            t.Check(result.decision == Decision::DropIncomplete && result.referenceLost && gate.IsWaiting(),
                    L"gate strict awaits IDR when loss may include another picture");

            gate.EnterWaiting();
            auto resume = MakeAu({ kIdrSlice });
            t.Check(gate.Process(*resume).decision == Decision::Submit, L"gate resumes on IDR");

            // An AU whose only slice was an aborted FU-A has no complete slice; it must still count
            // as a lost reference.
            auto noSurvivingSlice = std::make_unique<AccessUnit>();
            noSurvivingSlice->corrupt = true;
            noSurvivingSlice->sawSlice = true;
            noSurvivingSlice->allNonRef = false;
            result = gate.Process(*noSurvivingSlice);
            t.Check(result.decision == Decision::DropIncomplete && gate.IsWaiting(), L"gate awaits IDR after damaged AU with no complete slice");

            auto nothingSeen = std::make_unique<AccessUnit>();
            nothingSeen->corrupt = true;
            gate.EnterWaiting();
            gate.Process(*MakeAu({ kIdrSlice }));
            t.Check(gate.Process(*nothingSeen).decision == Decision::DropIncomplete && gate.IsWaiting(),
                    L"gate awaits IDR after damaged AU with unknown content");

            bool allDropped = true;
            for (int i = 0; i < 600; ++i)
            {
                allDropped = allDropped && gate.Process(*MakeAu({ kPSlice })).decision == Decision::DropAwaitingIdr;
            }
            t.Check(allDropped && gate.IsWaiting(), L"gate never resumes on non-IDR pictures");

            t.Check(gate.Process(*MakeAu({ kIdrSlice }, true)).decision == Decision::DropIncomplete && gate.IsWaiting(),
                    L"gate rejects damaged IDR");

            auto idrNoParams = MakeAu({ kIdrSlice });
            auto injected = gate.Process(*idrNoParams);
            bool const startsWithSps = idrNoParams->data.size() > 5 && idrNoParams->data[4] == 0x67;
            t.Check(injected.decision == Decision::Submit && injected.resynced && idrNoParams->hasSps && idrNoParams->hasPps &&
                    idrNoParams->nals.size() == 3 && idrNoParams->nals[0].type == 7 && idrNoParams->nals[1].type == 8 && startsWithSps,
                    L"gate prepends cached SPS/PPS to IDR");
            t.Check(gate.Process(*MakeAu({ kPSlice })).decision == Decision::Submit, L"gate decodes after IDR recovery");

            // Parameter sets delivered in their own AU are cached for a later bare IDR.
            H264KeyframeGate fresh;
            fresh.Configure(config);
            t.Check(fresh.Process(*MakeAu({ sps, kPps })).decision == Decision::DropNoSlice && fresh.HasParameterSets(),
                    L"gate caches SPS/PPS from a slice-less AU");
            t.Check(fresh.Process(*MakeAu({ kIdrSlice })).decision == Decision::Submit, L"gate submits bare IDR with cached SPS/PPS");
            H264KeyframeGate damaged;
            damaged.Configure(config);
            t.Check(damaged.Process(*MakeAu({ sps, kPps }, true)).decision == Decision::DropIncomplete && !damaged.HasParameterSets(),
                    L"gate ignores parameter sets from damaged AU");

            Bytes const sps720 = MakeSps(false, 80, 45, 0, false);
            t.Check(gate.Process(*MakeAu({ sps720, kPps, kPSlice })).decision == Decision::DropAwaitingIdr && gate.IsWaiting(),
                    L"gate awaits IDR on format change without IDR");
            auto resized = MakeAu({ sps720, kPps, kIdrSlice });
            auto resizedResult = gate.Process(*resized);
            t.Check(resizedResult.decision == Decision::Submit && !resizedResult.formatChanged && gate.Sps().width == 1280,
                    L"gate resumes on IDR in new format");
            Bytes const sps1080 = MakeSps(false, 120, 68, 4, false);
            t.Check(gate.Process(*MakeAu({ sps1080, kPps, kIdrSlice })).formatChanged, L"gate detects format change");

            // Tolerant: damaged pictures are dropped whole, complete ones keep decoding.
            H264KeyframeGate tolerant;
            H264KeyframeGate::Config tolerantConfig;
            tolerantConfig.policy = LossPolicy::Tolerant;
            tolerant.Configure(tolerantConfig);
            tolerant.Process(*MakeAu({ sps, kPps, kIdrSlice }));
            result = tolerant.Process(*MakeAu({ kPSlice }, true));
            t.Check(result.decision == Decision::DropIncomplete && result.referenceLost && !tolerant.IsWaiting(),
                    L"gate tolerant drops damaged picture without waiting");
            t.Check(tolerant.Process(*MakeAu({ kPSlice })).decision == Decision::Submit, L"gate tolerant keeps decoding complete pictures");
            tolerant.MarkUnprimed();
            t.Check(tolerant.Process(*MakeAu({ kPSlice })).decision == Decision::DropAwaitingIdr, L"gate tolerant needs IDR on a new decoder");
        }

        // Depacketizer + gate end to end: loss on a reference picture, AwaitingIDR, recovery.
        struct RecoveryRun
        {
            std::vector<Decision> decisions;
            std::vector<AccessUnitPtr> submitted;
            bool submittedOnlyComplete = true;
        };

        RecoveryRun RunRecovery(LossPolicy policy)
        {
            Bytes const sps = MakeSps(false, 120, 68, 4, false);
            RecoveryRun run;
            H264KeyframeGate gate;
            H264KeyframeGate::Config config;
            config.policy = policy;
            gate.Configure(config);

            DepackHarness h;
            auto drain = [&]()
            {
                for (auto& au : h.units)
                {
                    auto const result = gate.Process(*au);
                    run.decisions.push_back(result.decision);
                    if (result.decision == Decision::Submit)
                    {
                        run.submittedOnlyComplete = run.submittedOnlyComplete && !au->corrupt && !au->data.empty() &&
                            !au->nals.empty() && au->nals.front().offset == sizeof(h264::kStartCode);
                        run.submitted.push_back(std::move(au));
                    }
                }
                h.units.clear();
            };

            int64_t seq = 1000;
            h.Send(seq++, 1000, false, StapA({ sps, kPps }));
            seq = h.SendFragmented(seq, 1000, kIdrSliceLong, 3);     // IDR + SPS/PPS
            h.Send(seq++, 2500, true, kPSlice);                      // P
            seq = h.SendFragmented(seq, 4000, kPSliceLong, 2, { 1 }); // P, middle fragment lost
            h.Send(seq++, 5500, true, kPSlice);                      // P referencing the lost picture
            seq = h.SendFragmented(seq, 7000, kPSliceLong, 2);       // P
            seq = h.SendFragmented(seq, 8500, kIdrSliceLong, 3);     // IDR without SPS/PPS
            h.Send(seq++, 10000, true, kPSlice);                     // P
            drain();
            return run;
        }

        void TestVideoRecovery(TestContext& t)
        {
            RecoveryRun const strict = RunRecovery(LossPolicy::Strict);
            std::vector<Decision> const strictExpected{
                Decision::Submit, Decision::Submit, Decision::DropIncomplete, Decision::DropAwaitingIdr,
                Decision::DropAwaitingIdr, Decision::Submit, Decision::Submit };
            t.Check(strict.decisions == strictExpected, L"recovery strict: drop damaged P, await IDR, resume");
            t.Check(strict.submittedOnlyComplete && strict.submitted.size() == 4, L"recovery strict submits only complete AUs");
            if (strict.submitted.size() == 4)
            {
                AccessUnit const& recovered = *strict.submitted[2];
                t.Check(recovered.isIdr && recovered.discontinuity && recovered.nals.size() == 3 &&
                        recovered.nals[0].type == h264::kNalSps && recovered.nals[1].type == h264::kNalPps &&
                        NalBytes(recovered, 2) == kIdrSliceLong, L"recovery strict IDR carries cached SPS/PPS");
            }

            RecoveryRun const tolerant = RunRecovery(LossPolicy::Tolerant);
            std::vector<Decision> const tolerantExpected{
                Decision::Submit, Decision::Submit, Decision::DropIncomplete, Decision::Submit,
                Decision::Submit, Decision::Submit, Decision::Submit };
            t.Check(tolerant.decisions == tolerantExpected, L"recovery tolerant drops only the damaged AU");
            t.Check(tolerant.submittedOnlyComplete, L"recovery tolerant never submits incomplete AUs");
        }

        std::vector<VideoTimeline::Stamp> RunTimeline(VideoTimeline& timeline, int64_t start, std::vector<int64_t> const& steps)
        {
            // Frames arrive in real time with their RTP spacing.
            std::vector<VideoTimeline::Stamp> stamps;
            int64_t rtp = start;
            stamps.push_back(timeline.Next(rtp, 0));
            for (int64_t step : steps)
            {
                rtp += step;
                stamps.push_back(timeline.Next(rtp, VideoTimeline::RtpToTicks(rtp - start)));
            }
            return stamps;
        }

        bool StrictlyIncreasing(std::vector<VideoTimeline::Stamp> const& stamps)
        {
            for (size_t i = 1; i < stamps.size(); ++i)
            {
                if (stamps[i].pts <= stamps[i - 1].pts)
                {
                    return false;
                }
            }
            return true;
        }

        // pts spacing and durations follow the RTP deltas exactly (within rounding).
        bool FollowsRtp(std::vector<VideoTimeline::Stamp> const& stamps, std::vector<int64_t> const& steps, int64_t maxDuration)
        {
            int64_t cumulative = 0;
            for (size_t i = 0; i < steps.size(); ++i)
            {
                int64_t const before = VideoTimeline::RtpToTicks(cumulative);
                cumulative += steps[i];
                int64_t const expected = VideoTimeline::RtpToTicks(cumulative) - before;
                int64_t const delta = stamps[i + 1].pts - stamps[i].pts;
                int64_t const expectedDuration = std::min(VideoTimeline::RtpToTicks(steps[i]), maxDuration);
                if (delta != expected || std::llabs(stamps[i + 1].duration - expectedDuration) > 1)
                {
                    return false;
                }
            }
            return true;
        }

        void TestVideoTimeline(TestContext& t)
        {
            VideoTimeline::Config const config;
            struct Rate { int64_t step; int frames; wchar_t const* name; };
            for (Rate const rate : { Rate{ 1500, 60, L"timeline 60 fps spacing" }, Rate{ 1800, 50, L"timeline 50 fps spacing" },
                                     Rate{ 3000, 30, L"timeline 30 fps spacing" }, Rate{ 3750, 24, L"timeline 24 fps spacing" } })
            {
                VideoTimeline timeline;
                timeline.Configure(config);
                std::vector<int64_t> const steps(static_cast<size_t>(rate.frames), rate.step);
                auto const stamps = RunTimeline(timeline, 123456, steps);
                bool const oneSecond = stamps.back().pts - stamps.front().pts == VideoTimeline::kTicksPerSecond;
                t.Check(oneSecond && FollowsRtp(stamps, steps, config.maxDurationTicks) && StrictlyIncreasing(stamps) &&
                        timeline.Discontinuities() == 0, rate.name);
            }

            {
                // Variable spacing: 60/50/30 fps mixed with a static-screen pause.
                VideoTimeline timeline;
                timeline.Configure(config);
                std::vector<int64_t> const steps{ 1500, 1500, 1800, 1800, 3000, 1500, 45000, 1500, 3000, 1800 };
                auto const stamps = RunTimeline(timeline, 0, steps);
                t.Check(FollowsRtp(stamps, steps, config.maxDurationTicks) && StrictlyIncreasing(stamps) && timeline.Discontinuities() == 0,
                        L"timeline variable RTP spacing");
                t.Check(stamps[7].duration == config.maxDurationTicks && stamps[1].duration == 166667 && stamps[3].duration == 200000 &&
                        stamps[5].duration == 333333, L"timeline durations from adjacent RTP deltas");
                t.Check(stamps[0].duration == config.defaultDurationTicks && stamps[0].pts == 0, L"timeline first sample");
            }

            {
                // Rate switches between 24, 30, 50 and 60 fps keep RTP spacing and durations.
                VideoTimeline timeline;
                timeline.Configure(config);
                std::vector<int64_t> const steps{ 3750, 3750, 3000, 1800, 1500, 1500, 3750, 1800, 3000, 1500, 3750 };
                auto const stamps = RunTimeline(timeline, 42, steps);
                t.Check(FollowsRtp(stamps, steps, config.maxDurationTicks) && StrictlyIncreasing(stamps) && timeline.Discontinuities() == 0 &&
                        stamps[1].duration == 416667 && stamps[3].duration == 333333,
                        L"timeline mixed 24/30/50/60 fps spacing");
            }

            {
                // 32-bit RTP timestamp wrap through the unwrapper.
                VideoTimeline timeline;
                timeline.Configure(config);
                TimestampUnwrapper unwrap;
                std::vector<VideoTimeline::Stamp> stamps;
                uint32_t raw = 0xFFFFF000u;
                for (int i = 0; i < 10; ++i)
                {
                    stamps.push_back(timeline.Next(unwrap.Unwrap(raw), VideoTimeline::RtpToTicks(int64_t{ i } * 1500)));
                    raw += 1500u;
                }
                t.Check(StrictlyIncreasing(stamps) && stamps.back().pts - stamps.front().pts == VideoTimeline::RtpToTicks(9 * 1500) &&
                        timeline.Discontinuities() == 0, L"timeline RTP timestamp wrap");
            }

            {
                // Long run: no accumulated rounding drift.
                VideoTimeline timeline;
                timeline.Configure(config);
                int64_t first = 0;
                int64_t last = 0;
                for (int64_t i = 0; i < 100000; ++i)
                {
                    int64_t const pts = timeline.Next(i * 1500, VideoTimeline::RtpToTicks(i * 1500)).pts;
                    if (i == 0) first = pts;
                    last = pts;
                }
                t.Check(last - first == 16'666'500'000 && timeline.Discontinuities() == 0, L"timeline has no drift over 100k frames");
            }

            {
                // Epoch: the first sample gets the Starting position; RTP deltas follow from it.
                VideoTimeline timeline;
                timeline.Configure(config);
                timeline.Reset(12'345'678);
                auto const a = timeline.Next(4'000'000'000LL, 0);
                auto const b = timeline.Next(4'000'001'500LL, 166667);
                t.Check(a.pts == 12'345'678 && b.pts == 12'345'678 + 166667 && timeline.StartTicks() == 12'345'678,
                        L"timeline maps first RTP timestamp to the start position");
            }

            {
                // Repeated or backwards timestamps: new offset at lastPts + lastDuration.
                VideoTimeline timeline;
                timeline.Configure(config);
                auto const a = timeline.Next(9000, 0);
                auto const b = timeline.Next(9000, 166667);
                auto const c = timeline.Next(10500, 333333);
                auto const d = timeline.Next(6000, 500000);
                t.Check(a.pts == 0 && b.discontinuity && b.pts == a.pts + a.duration && !c.discontinuity && c.pts == b.pts + 166667 &&
                        d.discontinuity && d.pts == c.pts + c.duration && timeline.Discontinuities() == 2,
                        L"timeline backwards RTP starts new offset at last pts + duration");
            }

            {
                // A forward RTP jump far ahead of real arrival time is a discontinuity too.
                VideoTimeline timeline;
                timeline.Configure(config);
                auto const a = timeline.Next(0, 0);
                auto const b = timeline.Next(1500, 166667);
                int64_t const jumped = 1500 + 10 * 90000;
                auto const c = timeline.Next(jumped, 333333);
                auto const d = timeline.Next(jumped + 1500, 500000);
                t.Check(!a.discontinuity && !b.discontinuity && c.discontinuity && c.pts == b.pts + b.duration &&
                        !d.discontinuity && d.pts == c.pts + 166667 && timeline.Discontinuities() == 1,
                        L"timeline forward RTP jump starts new offset");
            }

            {
                // A static-screen pause keeps its real RTP length in pts; only the duration is capped.
                VideoTimeline timeline;
                timeline.Configure(config);
                std::vector<int64_t> const steps{ 1500, 1500, 5 * 90000, 1500 };
                auto const stamps = RunTimeline(timeline, 777, steps);
                t.Check(stamps[3].pts - stamps[2].pts == 50'000'000 && stamps[3].duration == config.maxDurationTicks &&
                        stamps[4].pts - stamps[3].pts == 166667 && timeline.Discontinuities() == 0,
                        L"timeline keeps pause length from RTP");
            }

            {
                // Reset starts the next source at zero again.
                VideoTimeline timeline;
                timeline.Configure(config);
                timeline.Next(123456, 0);
                timeline.Next(125000, 166667);
                timeline.Reset();
                auto const first = timeline.Next(999999, 999999);
                t.Check(first.pts == 0 && first.duration == config.defaultDurationTicks && timeline.Discontinuities() == 0,
                        L"timeline reset restarts at zero");
            }
        }

        // Access units for the delivery tests; reference frames unless nonRef.
        AccessUnitPtr DeliveryAu(int64_t rtp, Clock::time_point complete, bool idr = false, bool nonRef = false)
        {
            auto au = std::make_unique<AccessUnit>();
            au->rtpTimestamp = rtp;
            au->completeTime = complete;
            au->isIdr = idr;
            au->hasSlice = true;
            au->allNonRef = nonRef;
            au->data.assign(16, static_cast<uint8_t>(rtp & 0xFF));
            return au;
        }

        void TestFrameDelivery(TestContext& t)
        {
            using Delivery = FrameDelivery<int>;
            auto const t0 = Clock::now();
            size_t const unlimited = static_cast<size_t>(-1);

            {
                // Admission beyond a limit rejects the new frame and never touches queued ones.
                Delivery d;
                d.BeginSource(DeliveryAu(0, t0, true));
                for (int i = 1; i < 4; ++i)
                {
                    t.Check(d.Admit(DeliveryAu(i * 1500, t0), 4, unlimited), L"delivery admits below the frame limit");
                }
                t.Check(!d.Admit(DeliveryAu(6000, t0), 4, unlimited) && d.QueueSize() == 4, L"delivery rejects at the frame limit");
                t.Check(!d.Admit(DeliveryAu(6000, t0), 10, d.QueueBytes() + 8) && d.QueueSize() == 4, L"delivery rejects at the byte limit");
                auto first = d.OnRequest();
                t.Check(first && first->isIdr && first->rtpTimestamp == 0, L"delivery keeps the IDR first after rejections");
            }

            {
                // Overlapping requests are kept and served oldest first; a later request never
                // jumps ahead of them.
                Delivery d;
                d.BeginSource(DeliveryAu(0, t0, true));
                (void)d.OnRequest();
                t.Check(!d.OnRequest() && !d.Retain(1), L"delivery first retained request");
                t.Check(d.Retain(2) && d.PendingCount() == 2, L"delivery flags overlapping request");
                d.Admit(DeliveryAu(1500, t0), 10, unlimited);
                d.Admit(DeliveryAu(3000, t0), 10, unlimited);
                d.Admit(DeliveryAu(4500, t0), 10, unlimited);
                t.Check(!d.OnRequest(), L"delivery does not bypass retained requests");
                d.Retain(3);
                auto served = d.Serve();
                t.Check(served.size() == 3 && served[0].pending == 1 && served[1].pending == 2 && served[2].pending == 3 &&
                        served[0].au->rtpTimestamp == 1500 && served[2].au->rtpTimestamp == 4500, L"delivery serves requests FIFO");
            }

            {
                // Trimming and ageing remove only frames nothing references.
                Delivery d;
                d.BeginSource(DeliveryAu(0, t0, true));
                (void)d.OnRequest();
                d.Admit(DeliveryAu(1500, t0, false, true), 10, unlimited);
                d.Admit(DeliveryAu(3000, t0, false, true), 10, unlimited);
                d.Admit(DeliveryAu(4500, t0), 10, unlimited);
                d.Admit(DeliveryAu(6000, t0, false, true), 10, unlimited);
                t.Check(d.TrimNonReference(2) == 2 && d.QueueSize() == 2, L"delivery trims non-reference frames above soft depth");
                t.Check(d.DropStaleNonReference(t0 + std::chrono::seconds(1), std::chrono::milliseconds(100)) == 0,
                        L"delivery never ages out a reference frame at the front");
                auto a = d.OnRequest();
                t.Check(a && a->rtpTimestamp == 4500 && d.DropStaleNonReference(t0 + std::chrono::seconds(1), std::chrono::milliseconds(100)) == 1,
                        L"delivery ages out stale non-reference frames");
            }
        }

        AccessUnitPtr TimedAu(std::vector<Bytes> const& nals, int64_t rtp, Clock::time_point complete)
        {
            AccessUnitPtr au = MakeAu(nals);
            au->rtpTimestamp = rtp;
            au->completeTime = complete;
            au->firstPacketTime = complete;
            return au;
        }

        // Stream access units the gate accepts: IDR with SPS/PPS, or a reference P slice.
        AccessUnitPtr StreamAu(bool idr, int64_t rtp, Clock::time_point complete)
        {
            static Bytes const sps = MakeSps(false, 120, 68, 4, false);
            return idr ? TimedAu({ sps, kPps, kIdrSlice }, rtp, complete) : TimedAu({ kPSlice }, rtp, complete);
        }

        // Drives VideoDeliveryCore the way MediaStreamSource does: at most one outstanding
        // request, and a new one only after the previous request was satisfied.
        struct MssSim
        {
            explicit MssSim(VideoDeliveryCore& c) : core(c) {}

            void Absorb(VideoDeliveryCore::Output& out)
            {
                if (out.openSource)
                {
                    openedFormat = out.openSource->first;
                    source = out.openSource->second;
                    ++opened;
                }
                for (auto& completion : out.completions)
                {
                    if (outstanding && completion.requestSerial == outstandingSerial)
                    {
                        outstanding = false;
                    }
                    if (completion.sample)
                    {
                        samples.push_back(std::move(*completion.sample));
                    }
                    else
                    {
                        ++ended;
                    }
                }
            }

            void Push(AccessUnitPtr au, Clock::time_point now)
            {
                VideoDeliveryCore::Output out;
                core.OnAccessUnit(std::move(au), now, out);
                Absorb(out);
            }

            // Returns true when the request was satisfied immediately.
            bool Request(Clock::time_point now)
            {
                if (outstanding)
                {
                    return false;
                }
                VideoDeliveryCore::Output out;
                std::optional<VideoDeliveryCore::Sample> immediate;
                uint64_t const serial = ++nextSerial;
                auto const result = core.OnRequest(source, serial, now, immediate, out);
                if (result == VideoDeliveryCore::RequestResult::Immediate)
                {
                    samples.push_back(std::move(*immediate));
                }
                else if (result == VideoDeliveryCore::RequestResult::Retained)
                {
                    outstanding = true;
                    outstandingSerial = serial;
                }
                else
                {
                    ++stale;
                }
                Absorb(out);
                return result == VideoDeliveryCore::RequestResult::Immediate;
            }

            void ProcessAll()
            {
                for (auto const& s : samples)
                {
                    core.OnSampleProcessed(s.sourceId, s.sampleSerial);
                }
            }

            VideoDeliveryCore& core;
            uint64_t source = 0;
            uint64_t opened = 0;
            SpsInfo openedFormat;
            uint64_t nextSerial = 0;
            bool outstanding = false;
            uint64_t outstandingSerial = 0;
            uint64_t ended = 0;
            uint64_t stale = 0;
            std::vector<VideoDeliveryCore::Sample> samples;
        };

        // Every sample in RTP order at step spacing, pts exactly the RTP delta from the first.
        bool ContiguousSamples(std::vector<VideoDeliveryCore::Sample> const& samples, int64_t firstRtp, int64_t step, int64_t epoch = 0)
        {
            for (size_t i = 0; i < samples.size(); ++i)
            {
                int64_t const rtp = firstRtp + static_cast<int64_t>(i) * step;
                if (samples[i].au->rtpTimestamp != rtp || samples[i].pts != epoch + VideoTimeline::RtpToTicks(rtp - firstRtp) ||
                    samples[i].sampleSerial != i + 1)
                {
                    return false;
                }
            }
            return true;
        }

        Clock::time_point FrameTime(Clock::time_point t0, int64_t frame)
        {
            return t0 + std::chrono::microseconds(frame * 16667);
        }

        void TestVideoDeliveryCore(TestContext& t)
        {
            auto const t0 = Clock::now();
            VideoDeliveryCore::Config const defaults;

            {
                // The Xbox trace that stopped after five samples: IDR + 4 frames pulled, then
                // ~150 ms of decoder start-up with no requests while 60 fps frames arrive.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                int64_t frame = 0;
                mss.Push(StreamAu(true, 0, FrameTime(t0, frame)), FrameTime(t0, frame));
                core.OnStarting(mss.source, 0);
                for (frame = 1; frame < 5; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                }
                for (int i = 0; i < 5; ++i)
                {
                    mss.Request(FrameTime(t0, 5));
                }
                for (; frame < 14; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                }
                t.Check(mss.samples.size() == 5 && core.QueueSize() == 9 && !core.Gate().IsWaiting() &&
                        stats->Get(Stat::AccessUnitsDiscarded) == 0, L"core keeps frames through the start-up stall");
                t.Check(mss.Request(FrameTime(t0, 14)), L"core satisfies the sixth request immediately");
                while (mss.Request(FrameTime(t0, 14)))
                {
                }
                t.Check(mss.samples.size() == 14 && ContiguousSamples(mss.samples, 0, 1500) && mss.outstanding &&
                        stats->Get(Stat::PtsDiscontinuities) == 0, L"core delivers every frame after the stall in order");
                mss.Push(StreamAu(false, 14 * 1500, FrameTime(t0, 14)), FrameTime(t0, 14));
                t.Check(mss.samples.size() == 15 && !mss.outstanding, L"core serves the retained request with the next frame");
            }

            {
                // 180 frames arrive while the source first ignores and then requests slowly,
                // followed by normal demand.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                int64_t frame = 0;
                mss.Push(StreamAu(true, 0, FrameTime(t0, 0)), FrameTime(t0, 0));
                t.Check(mss.opened == 1 && core.CurrentPhase() == VideoDeliveryCore::Phase::Opening, L"core opens a source on the IDR");
                for (frame = 1; frame < 30; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                }
                t.Check(core.QueueSize() == 30 && mss.samples.empty() && stats->Get(Stat::DropStartupFull) == 0,
                        L"core holds IDR and GOP before the first request");
                core.OnStarting(mss.source, 0);
                for (; frame < 180; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                    if (frame % 10 == 0)
                    {
                        mss.Request(FrameTime(t0, frame));
                    }
                }
                bool const firstIsIdr = !mss.samples.empty() && mss.samples[0].keyframe && mss.samples[0].pts == 0 &&
                                        mss.samples[0].au->hasSps && mss.samples[0].au->hasPps && mss.samples[0].discontinuity;
                t.Check(firstIsIdr, L"core delivers the startup IDR with SPS/PPS first at pts 0");
                t.Check(mss.samples.size() == 15 && core.QueueSize() == 165 && !core.Gate().IsWaiting() &&
                        stats->Get(Stat::AccessUnitsDiscarded) == 0 && stats->Get(Stat::StartupPeakFrames) >= 165,
                        L"core keeps 165 frames of start-up backlog without drops");
                t.Check(core.CurrentPhase() == VideoDeliveryCore::Phase::Starting && core.Unprocessed() == 15,
                        L"core stays in start-up until a sample is rendered");

                core.OnSampleRendered(mss.source);
                t.Check(core.CurrentPhase() == VideoDeliveryCore::Phase::Playing && core.Draining(), L"core drains the start-up backlog");
                for (; frame < 480; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                    for (int r = 0; r < 3; ++r)
                    {
                        mss.Request(FrameTime(t0, frame));
                    }
                }
                t.Check(mss.samples.size() == 480 && ContiguousSamples(mss.samples, 0, 1500) && !core.Draining() &&
                        core.QueueSize() == 0, L"core submits all 480 frames in order after slow start-up");
                t.Check(stats->Get(Stat::AccessUnitsDiscarded) == 0 && stats->Get(Stat::DropAwaitingIdr) == 0 &&
                        stats->Get(Stat::IdrWaitsBackpressure) == 0 && stats->Get(Stat::PtsDiscontinuities) == 0,
                        L"core start-up without drops, IDR waits or pts discontinuities");
            }

            {
                // A bounded startup buffer: overflow rejects the newest pictures, keeps the IDR
                // and its GOP, and (Strict) awaits a new IDR for what was skipped.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                VideoDeliveryCore::Config config = defaults;
                config.startupCap = 20;
                core.Configure(config);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, t0), t0);
                for (int64_t frame = 1; frame < 30; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                }
                t.Check(core.QueueSize() == 20 && stats->Get(Stat::DropStartupFull) == 1 && stats->Get(Stat::DropAwaitingIdr) == 9 &&
                        stats->Get(Stat::IdrWaitsBackpressure) == 1 && stats->Get(Stat::IdrWaitsNetwork) == 0 && core.Gate().IsWaiting(),
                        L"core startup overflow skips newest frames and awaits IDR");
                while (mss.Request(FrameTime(t0, 30)))
                {
                }
                t.Check(mss.samples.size() == 20 && mss.samples[0].keyframe && ContiguousSamples(mss.samples, 0, 1500),
                        L"core startup overflow still delivers the IDR and its GOP");
                mss.Push(StreamAu(true, 40 * 1500, FrameTime(t0, 40)), FrameTime(t0, 40));
                mss.Push(StreamAu(false, 41 * 1500, FrameTime(t0, 41)), FrameTime(t0, 41));
                mss.Request(FrameTime(t0, 41));
                t.Check(mss.samples.size() == 22 && mss.samples[20].keyframe && !core.Gate().IsWaiting() && mss.opened == 1,
                        L"core resumes on the next IDR without a new source");
            }

            {
                // The start-up backlog drains down to exactly the playing cap and demand then
                // matches supply one for one: the cap handover must never reject a frame.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                VideoDeliveryCore::Config config = defaults;
                config.playingCap = 10;
                core.Configure(config);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, t0), t0);
                int64_t frame = 1;
                for (; frame < 25; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                }
                mss.Request(FrameTime(t0, frame));
                core.OnSampleRendered(mss.source);
                while (core.QueueSize() > 10)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                    mss.Request(FrameTime(t0, frame));
                    mss.Request(FrameTime(t0, frame));
                    ++frame;
                }
                for (int i = 0; i < 100; ++i, ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                    mss.Request(FrameTime(t0, frame));
                }
                while (mss.Request(FrameTime(t0, frame)))
                {
                }
                t.Check(mss.samples.size() == static_cast<size_t>(frame) && ContiguousSamples(mss.samples, 0, 1500) &&
                        stats->Get(Stat::AccessUnitsDiscarded) == 0 && !core.Gate().IsWaiting(),
                        L"core backlog at the playing cap never rejects a frame");
            }

            {
                // Damaged network input and back-pressure are counted separately.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, t0), t0);
                mss.Request(t0);
                auto damaged = MakeAu({ kPSlice }, true);
                damaged->rtpTimestamp = 1500;
                mss.Push(std::move(damaged), FrameTime(t0, 1));
                t.Check(stats->Get(Stat::IdrWaitsNetwork) == 1 && stats->Get(Stat::IdrWaitsBackpressure) == 0 && core.Gate().IsWaiting(),
                        L"core counts network damage as a network IDR wait");
            }

            {
                // Admission capacity is released by the pull: with the queue at the playing cap
                // a request frees a slot and the next frame is admitted; without it the frame is
                // rejected as back-pressure.
                VideoDeliveryCore::Config config = defaults;
                config.playingCap = 4;
                config.startupCap = 4;
                for (bool pull : { true, false })
                {
                    auto stats = std::make_shared<ReceiverStats>();
                    VideoDeliveryCore core(stats);
                    core.Configure(config);
                    MssSim mss(core);
                    mss.Push(StreamAu(true, 0, t0), t0);
                    mss.Request(t0);
                    core.OnSampleRendered(mss.source);
                    for (int64_t frame = 1; frame <= 4; ++frame)
                    {
                        mss.Push(StreamAu(false, frame * 1500, FrameTime(t0, frame)), FrameTime(t0, frame));
                    }
                    if (pull)
                    {
                        mss.Request(FrameTime(t0, 5));
                    }
                    mss.Push(StreamAu(false, 5 * 1500, FrameTime(t0, 5)), FrameTime(t0, 5));
                    if (pull)
                    {
                        t.Check(core.QueueSize() == 4 && stats->Get(Stat::DropQueueFull) == 0 && !core.Gate().IsWaiting(),
                                L"core pull releases admission capacity");
                    }
                    else
                    {
                        t.Check(core.QueueSize() == 4 && stats->Get(Stat::DropQueueFull) == 1 && stats->Get(Stat::IdrWaitsBackpressure) == 1,
                                L"core full queue without pull rejects as back-pressure");
                    }
                }
            }

            {
                // Processed accounting: each submitted sample is released exactly once.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, t0), t0);
                for (int64_t frame = 1; frame < 5; ++frame)
                {
                    mss.Push(StreamAu(false, frame * 1500, t0), t0);
                }
                for (int i = 0; i < 5; ++i)
                {
                    mss.Request(t0);
                }
                t.Check(core.Unprocessed() == 5 && stats->Get(Stat::SamplesInFlight) == 5, L"core counts five unprocessed samples");
                bool const released = core.OnSampleProcessed(mss.source, 1) && core.OnSampleProcessed(mss.source, 3);
                bool const repeated = core.OnSampleProcessed(mss.source, 3);
                bool const unknown = core.OnSampleProcessed(mss.source, 99) || core.OnSampleProcessed(mss.source + 7, 2);
                t.Check(released && !repeated && !unknown && core.Unprocessed() == 3, L"core Processed releases once, ignores repeats");
                mss.ProcessAll();
                t.Check(core.Unprocessed() == 0 && stats->Get(Stat::SamplesInFlight) == 0 && stats->Get(Stat::SamplesProcessed) == 5,
                        L"core Processed returns accounting to zero");
            }

            {
                // Epoch from Starting; stale requests end; source end releases retained requests.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 900000, t0), t0);
                core.OnStarting(mss.source, 5'000'000);
                mss.Push(StreamAu(false, 901500, FrameTime(t0, 1)), FrameTime(t0, 1));
                mss.Request(FrameTime(t0, 1));
                mss.Request(FrameTime(t0, 1));
                core.OnStarting(mss.source, 0);
                mss.Push(StreamAu(false, 903000, FrameTime(t0, 2)), FrameTime(t0, 2));
                mss.Request(FrameTime(t0, 2));
                t.Check(mss.samples.size() == 3 && ContiguousSamples(mss.samples, 900000, 1500, 5'000'000) && core.EpochTicks() == 5'000'000,
                        L"core epoch is the Starting position for the IDR's RTP timestamp");
                mss.Request(FrameTime(t0, 3));
                t.Check(mss.outstanding && core.PendingCount() == 1, L"core retains a request on an empty queue");
                VideoDeliveryCore::Output out;
                core.RequestNewSource(out);
                mss.Absorb(out);
                t.Check(mss.ended == 1 && !mss.outstanding && core.PendingCount() == 0, L"core ends retained request only when the source ends");
                mss.Request(FrameTime(t0, 3));
                t.Check(mss.stale == 1, L"core reports requests for a replaced source as stale");
                mss.Push(StreamAu(false, 904500, FrameTime(t0, 3)), FrameTime(t0, 3));
                t.Check(mss.opened == 1 && core.Gate().IsWaiting(), L"core needs an IDR for the next source");
                mss.Push(StreamAu(true, 906000, FrameTime(t0, 4)), FrameTime(t0, 4));
                mss.Request(FrameTime(t0, 4));
                t.Check(mss.opened == 2 && mss.samples.size() == 4 && mss.samples[3].pts == 0 && mss.samples[3].sampleSerial == 1,
                        L"core new source generation restarts the epoch at zero");
            }
        }

        // The SPS the decoder sees first in a submitted IDR (injected or carried).
        SpsInfo LeadingSps(VideoDeliveryCore::Sample const& sample)
        {
            SpsInfo info;
            AccessUnit const& au = *sample.au;
            if (!au.nals.empty() && au.nals[0].type == h264::kNalSps)
            {
                ParseSps(au.data.data() + au.nals[0].offset, au.nals[0].size, info);
            }
            return info;
        }

        void TestFormatRecovery(TestContext& t)
        {
            auto const t0 = Clock::now();
            VideoDeliveryCore::Config const defaults;
            Bytes const sps720 = MakeSps(false, 80, 45, 0, false);
            Bytes const sps1080 = MakeSps(false, 120, 68, 4, false);
            auto at = [&](int64_t frame) { return FrameTime(t0, frame); };

            {
                // 720p source; a parameter-set-only AU switches the cached SPS to 1080p and the
                // next IDR carries no SPS. That IDR must open a 1080p source.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(TimedAu({ sps720, kPps, kIdrSlice }, 0, at(0)), at(0));
                mss.Request(at(0));
                for (int64_t f = 1; f < 5; ++f)
                {
                    mss.Push(TimedAu({ kPSlice }, f * 1500, at(f)), at(f));
                    mss.Request(at(f));
                }
                uint64_t const firstSource = mss.source;
                bool const opened720 = mss.opened == 1 && mss.openedFormat.width == 1280 && mss.openedFormat.height == 720;

                mss.Push(TimedAu({ sps1080, kPps }, 5 * 1500, at(5)), at(5));
                t.Check(opened720 && mss.opened == 1 && core.SourceId() == firstSource &&
                        core.CurrentPhase() == VideoDeliveryCore::Phase::Starting && core.Gate().Sps().width == 1920,
                        L"format: SPS-only AU updates the cache without opening a source");

                mss.Push(TimedAu({ kPSlice }, 6 * 1500, at(6)), at(6));
                mss.Request(at(6));
                t.Check(mss.samples.size() == 5 && core.Gate().IsWaiting() && stats->Get(Stat::DropAwaitingIdr) == 1 && mss.outstanding,
                        L"format: a picture after the switch never reaches the old source");

                mss.Push(TimedAu({ kIdrSlice }, 7 * 1500, at(7)), at(7));
                mss.Request(at(7));
                bool const rebuilt = mss.opened == 2 && mss.source != firstSource && mss.openedFormat.width == 1920 &&
                                     mss.openedFormat.height == 1080 && mss.ended == 1;
                bool const idrFirst = mss.samples.size() == 6 && mss.samples[5].keyframe && mss.samples[5].sourceId == mss.source &&
                                      mss.samples[5].pts == 0 && LeadingSps(mss.samples[5]).width == 1920;
                t.Check(rebuilt && idrFirst, L"format: IDR after an SPS-only change opens a 1080p source with the cached SPS");

                mss.Push(TimedAu({ kPSlice }, 8 * 1500, at(8)), at(8));
                mss.Request(at(8));
                t.Check(mss.samples.size() == 7 && mss.samples[6].sourceId == mss.source && mss.opened == 2 && !core.Gate().IsWaiting(),
                        L"format: the new source keeps decoding after the rebuild");
            }

            {
                // An identical SPS repeated on its own, followed by a bare IDR, keeps the source.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(TimedAu({ sps1080, kPps, kIdrSlice }, 0, at(0)), at(0));
                mss.Request(at(0));
                for (int64_t f = 1; f < 20; ++f)
                {
                    if (f == 10)
                    {
                        mss.Push(TimedAu({ sps1080, kPps }, f * 1500, at(f)), at(f));
                        mss.Push(TimedAu({ kIdrSlice }, f * 1500, at(f)), at(f));
                    }
                    else
                    {
                        mss.Push(TimedAu({ kPSlice }, f * 1500, at(f)), at(f));
                    }
                    mss.Request(at(f));
                }
                t.Check(mss.opened == 1 && stats->Get(Stat::SourceBuilds) == 1 && mss.samples.size() == 20 &&
                        ContiguousSamples(mss.samples, 0, 1500) && stats->Get(Stat::AccessUnitsDiscarded) == 0,
                        L"format: same-resolution SPS refresh keeps the source");
            }

            {
                // SPS, PPS and IDR in one AU switch the source in both directions.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(TimedAu({ sps720, kPps, kIdrSlice }, 0, at(0)), at(0));
                mss.Request(at(0));
                mss.Push(TimedAu({ kPSlice }, 1500, at(1)), at(1));
                mss.Request(at(1));
                mss.Push(TimedAu({ sps1080, kPps, kIdrSlice }, 3000, at(2)), at(2));
                mss.Request(at(2));
                bool const to1080 = mss.opened == 2 && mss.openedFormat.width == 1920 && mss.samples.size() == 3 &&
                                    mss.samples[2].keyframe && mss.samples[2].pts == 0;
                mss.Push(TimedAu({ kPSlice }, 4500, at(3)), at(3));
                mss.Request(at(3));
                mss.Push(TimedAu({ sps720, kPps, kIdrSlice }, 6000, at(4)), at(4));
                mss.Request(at(4));
                t.Check(to1080 && mss.opened == 3 && mss.openedFormat.width == 1280 && mss.samples.size() == 5 &&
                        stats->Get(Stat::DropAwaitingIdr) == 0, L"format: SPS/PPS/IDR in one AU switches the source");
            }

            {
                // Periodic IDRs with the same SPS never rebuild.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                for (int64_t f = 0; f < 60; ++f)
                {
                    bool const idr = f % 10 == 0;
                    mss.Push(idr ? TimedAu({ sps1080, kPps, kIdrSlice }, f * 1500, at(f)) : TimedAu({ kPSlice }, f * 1500, at(f)), at(f));
                    mss.Request(at(f));
                }
                size_t keyframes = 0;
                for (auto const& s : mss.samples)
                {
                    keyframes += s.keyframe ? 1 : 0;
                }
                t.Check(mss.opened == 1 && stats->Get(Stat::SourceBuilds) == 1 && mss.samples.size() == 60 && keyframes == 6 &&
                        ContiguousSamples(mss.samples, 0, 1500), L"format: repeated same-format IDRs never rebuild the source");
            }

            {
                // The descriptor declares the profile but not the level.
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(TimedAu({ sps1080, kPps, kIdrSlice }, 0, at(0)), at(0));
                mss.Request(at(0));
                mss.Push(TimedAu({ MakeSps(false, 120, 68, 4, false, 42), kPps, kIdrSlice }, 1500, at(1)), at(1));
                mss.Request(at(1));
                bool const levelKept = mss.opened == 1;
                mss.Push(TimedAu({ MakeSps(true, 120, 68, 4, false), kPps, kIdrSlice }, 3000, at(2)), at(2));
                mss.Request(at(2));
                t.Check(levelKept && mss.opened == 2 && mss.openedFormat.profileIdc == 100 && mss.openedFormat.width == 1920,
                        L"format: profile change rebuilds, level-only change keeps the source");
            }

            {
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                VideoDeliveryCore::Config config = defaults;
                config.rebuildOnFormatChange = false;
                core.Configure(config);
                MssSim mss(core);
                mss.Push(TimedAu({ sps720, kPps, kIdrSlice }, 0, at(0)), at(0));
                mss.Request(at(0));
                mss.Push(TimedAu({ sps1080, kPps }, 1500, at(1)), at(1));
                mss.Push(TimedAu({ kIdrSlice }, 1500, at(1)), at(1));
                mss.Request(at(1));
                t.Check(mss.opened == 1 && mss.samples.size() == 2, L"format: rebuildOnFormatChange off keeps the source");
            }
        }

        void TestNewStreamHandover(TestContext& t)
        {
            auto const t0 = Clock::now();
            VideoDeliveryCore::Config const defaults;
            auto at = [&](int64_t frame) { return FrameTime(t0, frame); };

            // After a handover nothing reaches a decoder until the new stream's first complete IDR,
            // which opens a fresh source at pts 0; callbacks for the old source change nothing.
            auto finishHandover = [&](VideoDeliveryCore& core, MssSim& mss, uint64_t oldSource, int64_t frame)
            {
                size_t const before = mss.samples.size();
                VideoDeliveryCore::Output staleOut;
                core.OnSampleRendered(oldSource);
                core.OnStarting(oldSource, 777);
                core.OnSourceClosed(oldSource, staleOut);
                std::optional<VideoDeliveryCore::Sample> immediate;
                bool const staleRequest = core.OnRequest(oldSource, 9999, at(frame), immediate, staleOut) ==
                                          VideoDeliveryCore::RequestResult::Stale && !immediate;
                mss.Push(StreamAu(false, frame * 1500, at(frame)), at(frame));
                bool const heldForIdr = mss.samples.size() == before && mss.opened == 1 &&
                                        core.CurrentPhase() == VideoDeliveryCore::Phase::None && staleOut.completions.empty() && !staleOut.openSource;
                mss.Push(StreamAu(true, (frame + 1) * 1500, at(frame + 1)), at(frame + 1));
                mss.Request(at(frame + 1));
                bool const reopened = mss.opened == 2 && mss.source != oldSource && mss.samples.size() == before + 1 &&
                                      mss.samples.back().keyframe && mss.samples.back().pts == 0 && mss.samples.back().sampleSerial == 1 &&
                                      mss.samples.back().sourceId == mss.source;
                return staleRequest && heldForIdr && reopened;
            };

            {
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, at(0)), at(0));
                core.OnStarting(mss.source, 0);
                mss.Request(at(0));
                core.OnSampleRendered(mss.source);
                for (int64_t f = 1; f < 30; ++f)
                {
                    mss.Push(StreamAu(false, f * 1500, at(f)), at(f));
                    mss.Request(at(f));
                }
                uint64_t const oldSource = mss.source;
                size_t const submitted = mss.samples.size();
                VideoDeliveryCore::Output out;
                core.OnNewStream(out);
                mss.Absorb(out);
                bool const ended = out.completions.empty() && core.CurrentPhase() == VideoDeliveryCore::Phase::None && core.QueueSize() == 0 &&
                                   core.PendingCount() == 0 && core.Gate().IsWaiting() && !core.Gate().HasParameterSets();
                bool const processedOnce = core.OnSampleProcessed(oldSource, 30) && !core.OnSampleProcessed(oldSource, 30);
                t.Check(submitted == 30 && ended && processedOnce, L"handover during playback ends the old source");
                t.Check(finishHandover(core, mss, oldSource, 40), L"handover during playback opens the new source on its IDR");
            }

            {
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, at(0)), at(0));
                for (int64_t f = 1; f < 30; ++f)
                {
                    mss.Push(StreamAu(false, f * 1500, at(f)), at(f));
                }
                uint64_t const oldSource = mss.source;
                VideoDeliveryCore::Output out;
                core.OnNewStream(out);
                mss.Absorb(out);
                t.Check(out.completions.empty() && core.QueueSize() == 0 && stats->Get(Stat::DropStale) == 30 &&
                        core.CurrentPhase() == VideoDeliveryCore::Phase::None && mss.samples.empty(),
                        L"handover during start-up buffering discards the old backlog");
                t.Check(finishHandover(core, mss, oldSource, 40), L"handover during start-up buffering opens the new source on its IDR");
            }

            {
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, at(0)), at(0));
                mss.Request(at(0));
                mss.Request(at(0));
                uint64_t const oldSource = mss.source;
                bool const waiting = mss.outstanding && core.PendingCount() == 1;
                VideoDeliveryCore::Output out;
                core.OnNewStream(out);
                mss.Absorb(out);
                bool const endedOnce = mss.ended == 1 && !mss.outstanding && core.PendingCount() == 0 &&
                                       stats->Get(Stat::EndOfStreamCompletions) == 1;
                VideoDeliveryCore::Output again;
                core.OnNewStream(again);
                mss.Absorb(again);
                t.Check(waiting && endedOnce && again.completions.empty() && mss.ended == 1,
                        L"handover completes an outstanding request exactly once");
                t.Check(finishHandover(core, mss, oldSource, 40), L"handover with an outstanding request opens the new source on its IDR");
            }

            {
                auto stats = std::make_shared<ReceiverStats>();
                VideoDeliveryCore core(stats);
                core.Configure(defaults);
                MssSim mss(core);
                mss.Push(StreamAu(true, 0, at(0)), at(0));
                mss.Request(at(0));
                mss.Push(StreamAu(false, 1500, at(1)), at(1));
                mss.Request(at(1));
                auto damaged = MakeAu({ kPSlice }, true);
                damaged->rtpTimestamp = 3000;
                mss.Push(std::move(damaged), at(2));
                mss.Request(at(2));
                bool const awaiting = core.Gate().IsWaiting() && mss.outstanding && stats->Get(Stat::IdrWaitsNetwork) == 1;
                uint64_t const oldSource = mss.source;
                VideoDeliveryCore::Output out;
                core.OnNewStream(out);
                mss.Absorb(out);
                t.Check(awaiting && mss.ended == 1 && !mss.outstanding && core.CurrentPhase() == VideoDeliveryCore::Phase::None,
                        L"handover during an IDR wait releases the waiting request");
                t.Check(finishHandover(core, mss, oldSource, 40), L"handover during an IDR wait recovers on the new stream's IDR");
            }
        }

        void TestDeliveryConfig(TestContext& t)
        {
            size_t const mib = 1024 * 1024;
            ReceiverSettings defaults;
            t.Check(VideoDeliveryCore::MakeConfig(defaults).maxBytes == 64 * mib && VideoDeliveryCore::Config{}.maxBytes == 64 * mib,
                    L"config default byte cap stays 64 MiB");

            ReceiverSettings settings;
            settings.frameQueueMaxBytes = 12 * 1024 * 1024;
            VideoDeliveryCore::Config const configured = VideoDeliveryCore::MakeConfig(settings);
            t.Check(configured.maxBytes == 12 * mib, L"config propagates the configured byte cap");

            ReceiverSettings low;
            low.frameQueueMaxBytes = 1;
            ReceiverSettings high;
            high.frameQueueMaxBytes = std::numeric_limits<int32_t>::max();
            size_t const minBytes = static_cast<size_t>(ReceiverSettings::kMinFrameQueueBytes);
            size_t const maxBytes = static_cast<size_t>(ReceiverSettings::kMaxFrameQueueBytes);
            bool const clampedConfig = VideoDeliveryCore::MakeConfig(low).maxBytes == minBytes &&
                                       VideoDeliveryCore::MakeConfig(high).maxBytes == maxBytes;
            low.Sanitize();
            high.Sanitize();
            t.Check(clampedConfig && low.frameQueueMaxBytes == ReceiverSettings::kMinFrameQueueBytes &&
                    high.frameQueueMaxBytes == ReceiverSettings::kMaxFrameQueueBytes, L"config byte cap is clamped to safe bounds");

            // The configured cap reaches admission: 1 MiB pictures while nothing is requested.
            Bytes bigSlice(mib, 0x5A);
            bigSlice[0] = 0x41;
            Bytes const sps = MakeSps(false, 120, 68, 4, false);
            auto const t0 = Clock::now();
            auto fill = [&](VideoDeliveryCore::Config const& config, std::shared_ptr<ReceiverStats> const& stats)
            {
                VideoDeliveryCore core(stats);
                core.Configure(config);
                MssSim mss(core);
                mss.Push(TimedAu({ sps, kPps, kIdrSlice }, 0, t0), t0);
                for (int64_t f = 1; f <= 20; ++f)
                {
                    mss.Push(TimedAu({ bigSlice }, f * 1500, FrameTime(t0, f)), FrameTime(t0, f));
                }
                return core.QueueSize();
            };
            auto capped = std::make_shared<ReceiverStats>();
            auto roomy = std::make_shared<ReceiverStats>();
            size_t const cappedFrames = fill(configured, capped);
            size_t const defaultFrames = fill(VideoDeliveryCore::MakeConfig(defaults), roomy);
            t.Check(cappedFrames == 12 && capped->Get(Stat::DropStartupFull) == 1 && defaultFrames == 21 &&
                    roomy->Get(Stat::DropStartupFull) == 0, L"config byte cap limits the delivery queue");
        }

        void TestVideoIntegration(TestContext& t)
        {
            // RTP packets through the jitter buffer, depacketizer, gate, delivery and timeline:
            // one IDR followed by 3000 P-frames, with a slow-starting consumer. Every frame must
            // be submitted in order without a second IDR.
            JitterBuffer::Config jitterConfig;
            jitterConfig.reorderWindow = 64;
            jitterConfig.capacity = 128;
            jitterConfig.timeout = std::chrono::milliseconds(10);
            PipelineHarness p(jitterConfig);

            auto stats = std::make_shared<ReceiverStats>();
            VideoDeliveryCore core(stats);
            core.Configure(VideoDeliveryCore::Config{});
            MssSim mss(core);

            Bytes const sps = MakeSps(false, 120, 68, 4, false);
            auto const t0 = Clock::now();
            uint16_t sequence = 65000;
            uint32_t const ts0 = 0xFFFF0000u;
            int64_t const frames = 3001;
            bool noEarlyDeadlock = true;

            for (int64_t frame = 0; frame < frames; ++frame)
            {
                auto const now = FrameTime(t0, frame);
                uint32_t const ts = ts0 + static_cast<uint32_t>(frame * 1500);
                if (frame == 0)
                {
                    p.Feed(sequence++, ts, false, sps, now);
                    p.Feed(sequence++, ts, false, kPps, now);
                    p.Feed(sequence++, ts, true, kIdrSlice, now);
                }
                else
                {
                    p.Feed(sequence++, ts, true, kPSlice, now);
                }
                for (auto& au : p.out.units)
                {
                    mss.Push(std::move(au), now);
                }
                p.out.units.clear();

                if (frame == 20)
                {
                    core.OnStarting(mss.source, 0);
                }
                if (frame >= 20 && frame < 170 && frame % 8 == 0)
                {
                    mss.Request(now);
                }
                if (frame == 170)
                {
                    noEarlyDeadlock = mss.samples.size() > 5;
                    core.OnSampleRendered(mss.source);
                }
                if (frame >= 170)
                {
                    for (int r = 0; r < 2; ++r)
                    {
                        mss.Request(now);
                    }
                }
            }
            while (mss.Request(FrameTime(t0, frames)))
            {
            }
            mss.ProcessAll();

            int64_t const firstRtp = mss.samples.empty() ? 0 : mss.samples[0].au->rtpTimestamp;
            size_t idrs = 0;
            for (auto const& s : mss.samples)
            {
                idrs += s.keyframe ? 1 : 0;
            }
            t.Check(noEarlyDeadlock && p.lost == 0 && p.out.depack.GetCounters().completeAccessUnits == static_cast<uint64_t>(frames),
                    L"integration receives 3001 complete access units");
            t.Check(mss.samples.size() == static_cast<size_t>(frames) && idrs == 1 && mss.samples[0].keyframe && mss.samples[0].pts == 0,
                    L"integration submits all frames after one IDR");
            t.Check(ContiguousSamples(mss.samples, firstRtp, 1500) && stats->Get(Stat::PtsDiscontinuities) == 0,
                    L"integration pts follow RTP across the timestamp wrap");
            t.Check(stats->Get(Stat::AccessUnitsDiscarded) == 0 && stats->Get(Stat::DropAwaitingIdr) == 0 && !core.Gate().IsWaiting() &&
                    mss.opened == 1 && mss.ended == 0, L"integration needs no second IDR or source");
            t.Check(core.Unprocessed() == 0 && stats->Get(Stat::SamplesProcessed) == frames, L"integration accounting returns to zero");
        }

        void TestAudio(TestContext& t)
        {
            uint8_t const minusOne[2] = { 0x80, 0x00 };
            uint8_t const maxPositive[2] = { 0x7F, 0xFF };
            uint8_t const smallest[2] = { 0x00, 0x01 };
            uint8_t const minusSmallest[2] = { 0xFF, 0xFF };
            t.Check(S16BeToFloat(minusOne) == -1.0f, L"l16 0x8000 -> -1.0");
            t.Check(std::fabs(S16BeToFloat(maxPositive) - 32767.0f / 32768.0f) < 1e-7f, L"l16 0x7FFF -> +0.99997");
            t.Check(std::fabs(S16BeToFloat(smallest) - 1.0f / 32768.0f) < 1e-9f, L"l16 0x0001");
            t.Check(std::fabs(S16BeToFloat(minusSmallest) + 1.0f / 32768.0f) < 1e-9f, L"l16 0xFFFF");

            uint8_t const stereo[4] = { 0x40, 0x00, 0xC0, 0x00 };
            float converted[2] = {};
            ConvertS16BeToFloat(stereo, 2, converted);
            t.Check(converted[0] == 0.5f && converted[1] == -0.5f, L"l16 channel order preserved");

            PcmRingBuffer ring(8, 2);
            float in[12] = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6 };
            float out[12] = {};
            ring.Write(in, 6);
            ring.Read(out, 4);
            size_t const written = ring.Write(in, 6);
            t.Check(written == 6 && ring.Available() == 8, L"ring wraps and caps at capacity");
            ring.Read(out, 3);
            t.Check(out[0] == 5.0f && out[2] == 6.0f && out[4] == 1.0f, L"ring preserves order across wrap");

            AvSyncInputs inputs;
            inputs.videoPipelineLatencyMs = 60;
            inputs.audioOutputLatencyMs = 10;
            t.Check(ComputeAudioTargetDelayMs(inputs) == 50, L"av sync target delay");
            inputs.userOffsetMs = -200;
            t.Check(ComputeAudioTargetDelayMs(inputs) == 20, L"av sync clamps to minimum");

            // Receiver: a timestamp gap is concealed with exactly the missing frames.
            auto stats = std::make_shared<ReceiverStats>();
            auto pcm = std::make_shared<PcmRingBuffer>(44100, 2);
            ReceiverSettings settings;
            AudioReceiver receiver(settings, stats, pcm);
            Bytes payload(400 * 4, 0x10);
            auto now = Clock::now();
            Bytes first = MakeRtp(10, 0, 0x55, false, payload);
            Bytes third = MakeRtp(12, 800, 0x55, false, payload);
            Bytes second = MakeRtp(11, 400, 0x55, false, payload);
            receiver.ProcessDatagram(first.data(), first.size(), now);
            receiver.ProcessDatagram(third.data(), third.size(), now);
            receiver.ProcessDatagram(second.data(), second.size(), now);
            t.Check(pcm->Available() == 1200 && stats->Get(Stat::AudioReordered) == 1, L"audio reorder without concealment");
            Bytes gapped = MakeRtp(14, 1600, 0x55, false, payload);
            receiver.ProcessDatagram(gapped.data(), gapped.size(), now + std::chrono::milliseconds(100));
            t.Check(pcm->Available() == 1200, L"audio holds packet after sequence gap");
            receiver.Poll(now + std::chrono::milliseconds(200));
            t.Check(stats->Get(Stat::AudioLost) == 1 && stats->Get(Stat::AudioConcealedFrames) == 400 && pcm->Available() == 2000,
                    L"audio conceals lost packet after reorder timeout");
        }

        void TestAudioActivity(TestContext& t)
        {
            auto stats = std::make_shared<ReceiverStats>();
            auto pcm = std::make_shared<PcmRingBuffer>(44100, 2);
            ReceiverSettings settings;
            AudioReceiver receiver(settings, stats, pcm);
            Bytes const payload(400 * 4, 0x10);
            auto feed = [&](Bytes const& datagram, Clock::time_point when) { receiver.ProcessDatagram(datagram.data(), datagram.size(), when); };

            auto const t1 = Clock::now();
            feed(Bytes{ 0x80, 0x60 }, t1);
            feed(MakeRtp(1, 0, 0x55, false, payload, 97), t1);
            feed(MakeRtp(1, 0, 0x55, false, Bytes(4000, 0x10)), t1);
            feed(MakeRtp(1, 0, 0x55, false, { 0x10, 0x20 }), t1);
            t.Check(!receiver.EverReceived() && stats->Get(Stat::AudioInvalid) == 3 && stats->Get(Stat::AudioWrongPayloadType) == 1 &&
                    stats->Get(Stat::AudioPackets) == 4 && pcm->Available() == 0, L"audio activity ignores rejected datagrams");

            feed(MakeRtp(10, 0, 0x55, false, payload), t1);
            bool const accepted = receiver.EverReceived() && receiver.LastPacketTime() == t1;
            auto const t2 = t1 + std::chrono::milliseconds(500);
            feed(Bytes{ 0x80, 0x60, 0x00 }, t2);
            feed(MakeRtp(11, 400, 0x55, false, payload, 97), t2);
            feed(MakeRtp(900, 0, 0x77, false, payload), t2);
            t.Check(accepted && receiver.LastPacketTime() == t1 && stats->Get(Stat::AudioForeignSsrc) == 1,
                    L"audio idle timer is reset only by accepted packets");
        }

        // Feeds 400-frame L16 packets to one receiver and reports what each produced. The ring is
        // drained after every packet, so Written is exactly that packet's output.
        class AudioTimelineHarness
        {
        public:
            static constexpr uint32_t kFrames = 400;

            AudioTimelineHarness() :
                stats(std::make_shared<ReceiverStats>()),
                pcm(std::make_shared<PcmRingBuffer>(44100, 2)),
                receiver(ReceiverSettings{}, stats, pcm)
            {
            }

            size_t Send(uint16_t sequence, uint32_t timestamp, uint32_t ssrc = 0x55)
            {
                Bytes const packet = MakeRtp(sequence, timestamp, ssrc, false, Bytes(kFrames * 4, 0x10));
                receiver.ProcessDatagram(packet.data(), packet.size(), now);
                now += std::chrono::microseconds(9070);
                size_t const written = pcm->Available();
                pcm->Discard(written);
                return written;
            }

            // Sends count in-order packets continuing both counters; true if each wrote a full packet.
            bool Continue(uint16_t& sequence, uint32_t& timestamp, int count, uint32_t ssrc = 0x55)
            {
                bool full = true;
                for (int i = 0; i < count; ++i)
                {
                    full = Send(sequence++, timestamp, ssrc) == kFrames && full;
                    timestamp += kFrames;
                }
                return full;
            }

            int64_t Get(Stat stat) const { return stats->Get(stat); }

            std::shared_ptr<ReceiverStats> stats;
            std::shared_ptr<PcmRingBuffer> pcm;
            AudioReceiver receiver;
            Clock::time_point now = Clock::now();
        };

        void TestAudioTimestampReset(TestContext& t)
        {
            constexpr uint32_t kThreshold = 44100 / 5;  // 200 ms of frames
            uint32_t constexpr kFrames = AudioTimelineHarness::kFrames;

            {
                // Below the threshold: an overlap, not a reset. The packet is late and nothing resyncs.
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 100000;
                bool const warm = h.Continue(seq, ts, 3);
                h.pcm->ConsumeFlushRequest();
                size_t const written = h.Send(seq++, ts - 4000);
                t.Check(warm && written == 0 && h.Get(Stat::AudioLate) == 1 && h.Get(Stat::AudioDiscontinuities) == 0 &&
                            !h.pcm->ConsumeFlushRequest(),
                        L"audio ts reset: a backward step below 200 ms is late, not a reset");
            }
            {
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 100000;
                h.Continue(seq, ts, 3);
                size_t const written = h.Send(seq++, ts - kThreshold);
                t.Check(written == 0 && h.Get(Stat::AudioLate) == 1 && h.Get(Stat::AudioDiscontinuities) == 0,
                        L"audio ts reset: a backward step of exactly 200 ms is not a reset");
            }
            {
                // One frame beyond the threshold resynchronizes and plays the packet at once.
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 100000;
                h.Continue(seq, ts, 3);
                h.pcm->ConsumeFlushRequest();
                uint32_t reset = ts - kThreshold - 1;
                size_t const first = h.Send(seq++, reset);
                bool const flushed = h.pcm->ConsumeFlushRequest();
                reset += kFrames;
                bool const following = h.Continue(seq, reset, 20);
                t.Check(first == kFrames && flushed && h.Get(Stat::AudioDiscontinuities) == 1 && h.Get(Stat::AudioLate) == 0 &&
                            following && h.Get(Stat::AudioConcealedFrames) == 0,
                        L"audio ts reset: a backward step beyond 200 ms resyncs on the first packet");
                t.Check(h.Get(Stat::AudioStreamRestarts) == 0 && h.Get(Stat::AudioLost) == 0,
                        L"audio ts reset: same SSRC with continuing sequence is not a stream restart");
            }
            {
                // The reported failure: the timestamp jumps back ten seconds and audio used to stay
                // silent until the old timeline caught up.
                AudioTimelineHarness h;
                uint16_t seq = 60000;
                uint32_t ts = 5000000;
                h.Continue(seq, ts, 5);
                uint32_t reset = ts - 441000;
                bool const recovered = h.Continue(seq, reset, 200);
                t.Check(recovered && h.Get(Stat::AudioLate) == 0 && h.Get(Stat::AudioDiscontinuities) == 1,
                        L"audio ts reset: ten seconds backward recovers immediately across sequence wrap, one resync only");
            }
            {
                // A reset lands in the middle of normal reordering: the jitter buffer still orders by
                // sequence, and the reset is applied once when the first new-timeline packet plays.
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 100000;
                h.Continue(seq, ts, 3);
                uint32_t const reset = 7;
                size_t const ahead = h.Send(static_cast<uint16_t>(seq + 1), reset + kFrames);
                size_t const filled = h.Send(seq, reset);
                seq += 2;
                uint32_t next = reset + 2 * kFrames;
                bool const following = h.Continue(seq, next, 10);
                t.Check(ahead == 0 && filled == 2 * kFrames && following && h.Get(Stat::AudioReordered) == 1 &&
                            h.Get(Stat::AudioDiscontinuities) == 1 && h.Get(Stat::AudioLate) == 0,
                        L"audio ts reset: reordering around a reset still plays in sequence order");
            }
            {
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 0xFFFFFFFFu - 3 * kFrames + 1;
                bool const across = h.Continue(seq, ts, 12);
                t.Check(across && h.Get(Stat::AudioDiscontinuities) == 0 && h.Get(Stat::AudioLate) == 0 &&
                            h.Get(Stat::AudioConcealedFrames) == 0,
                        L"audio ts reset: 32-bit timestamp wrap is not mistaken for a reset");
            }
            {
                // Forward behavior is unchanged: a short gap is concealed, a long one flushes and plays.
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 100000;
                h.Continue(seq, ts, 3);
                size_t const shortGap = h.Send(seq++, ts + 800);
                ts += 800 + kFrames;
                h.pcm->ConsumeFlushRequest();
                size_t const atLimit = h.Send(seq++, ts + kThreshold);
                ts += kThreshold + kFrames;
                bool const limitNoFlush = !h.pcm->ConsumeFlushRequest();
                size_t const longGap = h.Send(seq++, ts + kThreshold + 1);
                t.Check(shortGap == 800 + kFrames && atLimit == kThreshold + kFrames && limitNoFlush &&
                            longGap == kFrames && h.pcm->ConsumeFlushRequest() && h.Get(Stat::AudioDiscontinuities) == 1 &&
                            h.Get(Stat::AudioConcealedFrames) == 800 + kThreshold,
                        L"audio ts reset: forward gaps conceal up to 200 ms and flush beyond it");
            }
            {
                // A new SSRC with an earlier timestamp is a stream restart, handled by takeover probation.
                AudioTimelineHarness h;
                uint16_t seq = 1;
                uint32_t ts = 5000000;
                h.Continue(seq, ts, 3);
                uint16_t otherSeq = 900;
                uint32_t otherTs = 1000;
                size_t const probation = h.Send(otherSeq++, otherTs, 0x66);
                otherTs += kFrames;
                bool const switched = h.Continue(otherSeq, otherTs, 10, 0x66);
                t.Check(probation == 0 && switched && h.Get(Stat::AudioStreamRestarts) == 1 && h.Get(Stat::AudioForeignSsrc) == 1 &&
                            h.Get(Stat::AudioDiscontinuities) == 0 && h.Get(Stat::AudioLate) == 0,
                        L"audio ts reset: a changed SSRC restarts the stream without a timestamp resync");
            }
        }

        // Largest deviation from an ideal sine when resampling 44.1 kHz to 48 kHz. Images and
        // aliases add to the error, so a small value also means good stopband rejection.
        double ResampleSineError(SincResampler const& resampler, double hz, uint32_t channels)
        {
            constexpr double kPi = 3.14159265358979323846;
            double const ratio = 44100.0 / 48000.0;
            size_t const inputFrames = 4096;
            std::vector<float> stage(inputFrames * channels);
            for (size_t n = 0; n < inputFrames; ++n)
            {
                float const v = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * n / 44100.0));
                for (uint32_t c = 0; c < channels; ++c)
                {
                    stage[n * channels + c] = c == 0 ? v : -v;
                }
            }
            double const start = 100.25;
            size_t const outputFrames = 3000;
            std::vector<float> out(outputFrames * channels);
            resampler.Process(stage.data(), channels, start, ratio, out.data(), outputFrames);

            double worst = 0.0;
            for (size_t k = 0; k < outputFrames; ++k)
            {
                double const p = start + k * ratio;
                double const ideal = 0.5 * std::sin(2.0 * kPi * hz * p / 44100.0);
                worst = std::max(worst, std::fabs(out[k * channels] - ideal));
                if (channels > 1)
                {
                    worst = std::max(worst, static_cast<double>(std::fabs(out[k * channels + 1] + out[k * channels])));
                }
            }
            return worst;
        }

        void TestResampler(TestContext& t)
        {
            SincResampler resampler;
            resampler.Configure(44100.0 / 48000.0);

            std::vector<float> dc(256, 0.5f);
            std::vector<float> out(64);
            resampler.Process(dc.data(), 1, 50.37, 44100.0 / 48000.0, out.data(), out.size());
            bool flat = true;
            for (float v : out)
            {
                flat = flat && std::fabs(v - 0.5f) < 1e-5f;
            }
            t.Check(flat, L"resampler unity DC gain");

            double const e1k = ResampleSineError(resampler, 1000.0, 2);
            double const e10k = ResampleSineError(resampler, 10000.0, 1);
            double const e16k = ResampleSineError(resampler, 16000.0, 1);
            Log(L"resampler max error: 1 kHz %.2e, 10 kHz %.2e, 16 kHz %.2e (full scale 0.5)", e1k, e10k, e16k);
            t.Check(e1k < 5e-4, L"resampler 1 kHz within -60 dB, stereo channels independent");
            t.Check(e10k < 5e-4, L"resampler 10 kHz within -60 dB");
            t.Check(e16k < 5e-4, L"resampler 16 kHz within -60 dB");
        }

        void TestAudioStage(TestContext& t)
        {
            // Render reads FramesNeeded stage frames from a position in [kHistory, kHistory + 1); the
            // stage preallocated for a render size must cover it at every ratio the drift loop uses.
            bool covered = true;
            for (double const base : { 44100.0 / 48000.0, 1.0, 44100.0 / 32000.0 })
            {
                double const maxRatio = base * 1.005;
                for (size_t const frames : { size_t{ 1 }, size_t{ 441 }, size_t{ 480 }, size_t{ 1024 }, size_t{ 3840 } })
                {
                    size_t const capacity = SincResampler::StageFrames(frames, maxRatio);
                    for (double const ratio : { base * 0.995, base, maxRatio })
                    {
                        for (double const position : { static_cast<double>(SincResampler::kHistory), SincResampler::kHistory + 0.999999 })
                        {
                            covered = covered && SincResampler::FramesNeeded(position, frames, ratio) <= capacity;
                        }
                    }
                }
            }
            t.Check(covered, L"audio stage capacity covers every render up to the quantum bound");
        }

        // The delay control of AudioPresenter::Render with the real step and drift classes: 44.1 kHz
        // in, 48 kHz out, 10 ms quanta. The sender adds one quantum of input after each render.
        struct PlayoutModel
        {
            static constexpr double kInPerMs = 44.1;
            static constexpr double kOutPerMs = 48.0;
            static constexpr size_t kQuantum = 480;

            // Starts in steady playout: one quantum has already rendered against the target.
            explicit PlayoutModel(int32_t targetMs = 60) : target(targetMs), fillMs(targetMs) { Quantum(); }

            // Returns output frames of silence inserted by a manual step in this quantum.
            size_t Quantum()
            {
                AudioTargetDelay::Sample const s = target.Load();
                size_t const discard = step.Update(s, primed, kInPerMs);
                discarded += discard;
                fillMs = std::max(0.0, fillMs - discard / kInPerMs);
                errorMs = fillMs - s.targetMs + step.PendingHoldMs();
                correction = drift.Update(errorMs, now);
                maxAbsCorrection = std::max(maxAbsCorrection, std::fabs(correction));
                size_t const hold = step.TakeHold(kQuantum, kOutPerMs);
                held += hold;
                fillMs -= (kQuantum - hold) / kOutPerMs * (1.0 + correction);
                fillMs += 10.0 + senderExcessMs;
                now += std::chrono::milliseconds(10);
                return hold;
            }

            void Run(int quanta)
            {
                for (int i = 0; i < quanta; ++i)
                {
                    Quantum();
                }
            }

            bool Near(double ms) const { return std::fabs(fillMs - ms) < 0.05; }

            AudioTargetDelay target;
            ManualDelayStep step;
            DriftTrim drift;
            double fillMs;
            bool primed = true;
            double senderExcessMs = 0.0;
            Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);
            size_t held = 0;
            size_t discarded = 0;
            double errorMs = 0.0;
            double correction = 0.0;
            double maxAbsCorrection = 0.0;
        };

        void TestManualAvOffset(TestContext& t)
        {
            {
                PlayoutModel m;
                m.target.Step(70);
                size_t const first = m.Quantum();
                size_t const second = m.Quantum();
                t.Check(first == 480 && second == 0 && m.held == 480 && m.discarded == 0 && m.Near(70.0),
                        L"av offset step: +10 ms inserts 10 ms of silence in the next quantum");
            }
            {
                PlayoutModel m;
                m.target.Step(50);
                size_t const hold = m.Quantum();
                t.Check(hold == 0 && m.discarded == 441 && m.Near(50.0), L"av offset step: -10 ms drops 10 ms of input in the next quantum");
            }
            {
                PlayoutModel m;
                m.target.Step(160);
                bool const startsNow = m.Quantum() == 480;
                m.Run(9);
                bool const done = m.step.PendingHoldMs() == 0.0 && m.held == 4800;
                m.Run(50);
                t.Check(startsNow && done && m.held == 4800 && m.discarded == 0 && m.Near(160.0) && m.maxAbsCorrection == 0.0,
                        L"av offset step: +100 ms is a bounded 100 ms gap, not a drift correction");
            }
            {
                // +50 then, mid-transition, -50: the result is the latest target, not the sum of moves.
                PlayoutModel m(150);
                m.target.Step(200);
                size_t const firstHold = m.Quantum();
                m.target.Step(100);
                size_t const secondHold = m.Quantum();
                m.Run(20);
                t.Check(firstHold == 480 && secondHold == 0 && m.discarded == 2646 && m.Near(100.0),
                        L"av offset step: a sign change mid-transition lands on the latest target");
            }
            {
                PlayoutModel m;
                m.target.Step(70);
                m.target.Step(80);
                m.target.Step(90);
                m.Run(10);
                PlayoutModel back;
                back.target.Step(70);
                back.target.Step(60);
                back.Run(10);
                t.Check(m.held == 1440 && m.discarded == 0 && m.Near(90.0) && back.held == 0 && back.discarded == 0 && back.Near(60.0),
                        L"av offset step: rapid presses are combined into one move to the latest target");
            }
            {
                PlayoutModel same;
                same.target.Step(60);
                same.Run(5);
                PlayoutModel ordinary;
                ordinary.target.Set(80);
                ordinary.Run(5);
                t.Check(same.held == 0 && same.discarded == 0 && same.Near(60.0) && ordinary.held == 0 && ordinary.discarded == 0,
                        L"av offset step: an unchanged value, or ordinary target movement, inserts or drops nothing");
            }
            {
                PlayoutModel priming;
                priming.primed = false;
                priming.target.Step(160);
                priming.Run(3);
                t.Check(priming.held == 0 && priming.discarded == 0 && priming.step.PendingHoldMs() == 0.0,
                        L"av offset step: while priming the new target needs no extra silence");
            }
            {
                AudioTargetDelay delay(60);
                delay.Step(5000);
                int32_t const high = delay.Load().targetMs;
                delay.Step(-20);
                int32_t const low = delay.Load().targetMs;
                ManualDelayStep step;
                AudioTargetDelay extreme(0);
                step.Update(extreme.Load(), true, 44.1);
                extreme.Step(1000);
                step.Update(extreme.Load(), true, 44.1);
                bool const bounded = step.PendingHoldMs() == 1000.0;

                AvSyncInputs inputs;
                inputs.userOffsetMs = 500;
                int32_t const most = ComputeAudioTargetDelayMs(inputs);
                inputs.userOffsetMs = -500;
                int32_t const least = ComputeAudioTargetDelayMs(inputs);
                ReceiverSettings wild;
                wild.avOffsetMs = -9000;
                wild.Sanitize();
                t.Check(high == AudioTargetDelay::kMaxMs && low == 0 && bounded && most == 300 && least == 20 && wild.avOffsetMs == -500,
                        L"av offset step: targets, steps and saved offsets stay inside their bounds");
            }
            {
                // The deadband that hid a 10 ms manual change still guards ordinary drift.
                DriftTrim trim;
                auto when = Clock::time_point{} + std::chrono::hours(1);
                double correction = 0.0;
                for (int i = 0; i < 1000; ++i)
                {
                    correction = std::max(correction, std::fabs(trim.Update(10.0, when)));
                    when += std::chrono::milliseconds(10);
                }
                t.Check(correction == 0.0, L"av offset step: a steady 10 ms error stays inside the drift deadband");
            }
            {
                // After a manual step, a sender clock 0.2 % fast is still trimmed, then released.
                PlayoutModel m;
                m.target.Step(160);
                m.Run(20);
                bool const quiet = m.maxAbsCorrection == 0.0 && m.Near(160.0);
                m.senderExcessMs = 0.02;
                m.Run(1500);
                bool const trimming = m.correction > 0.0 && m.errorMs < 25.0;
                m.senderExcessMs = 0.0;
                m.Run(1500);
                t.Check(quiet && trimming && m.correction == 0.0 && std::fabs(m.errorMs) < DriftTrim::kDeadbandMs,
                        L"av offset step: drift correction resumes after the manual transition");
            }
            {
                IPropertySet values = PropertySet();
                ReceiverSettings written;
                written.avOffsetMs = -10;
                written.WriteToValues(values);
                ReceiverSettings loaded = ReceiverSettings::LoadFromValues(values);
                loaded.Sanitize();
                bool const negative = loaded.avOffsetMs == -10 && unbox_value_or<int32_t>(values.TryLookup(L"avOffsetMs"), 0) == -10;
                written.avOffsetMs = 10;
                written.WriteToValues(values);
                loaded = ReceiverSettings::LoadFromValues(values);
                loaded.Sanitize();
                t.Check(negative && loaded.avOffsetMs == 10 && ReceiverSettings::kCurrentSettingsVersion == 3 &&
                            unbox_value_or<int32_t>(values.TryLookup(L"settingsVersion"), 0) == 3,
                        L"av offset step: saved offset key, sign and schema version are unchanged");
            }
        }

        IPropertySet MakeSettingsMap()
        {
            return PropertySet();
        }

        void TestReceiverSettings(TestContext& t)
        {
            IPropertySet empty = MakeSettingsMap();
            ReceiverSettings clean = ReceiverSettings::LoadFromValues(empty);
            clean.Sanitize();
            t.Check(!clean.diagnosticsVisible && !clean.firstRunDismissed && clean.avOffsetMs == 0 &&
                        clean.videoPort == 5000 && clean.audioPort == 5002,
                    L"settings: clean install defaults");

            IPropertySet migratedClean = MakeSettingsMap();
            ReceiverSettings::MigrateStoredSettings(migratedClean);
            ReceiverSettings migratedDefaults = ReceiverSettings::LoadFromValues(migratedClean);
            migratedDefaults.Sanitize();
            t.Check(!migratedDefaults.diagnosticsVisible && !migratedDefaults.firstRunDismissed &&
                        unbox_value_or<int32_t>(migratedClean.TryLookup(L"settingsVersion"), 0) ==
                            ReceiverSettings::kCurrentSettingsVersion,
                    L"settings: clean migration stamps schema version without changing defaults");

            IPropertySet current = MakeSettingsMap();
            ReceiverSettings written;
            written.diagnosticsVisible = true;
            written.firstRunDismissed = true;
            written.avOffsetMs = 100;
            written.videoPort = 5010;
            written.audioPort = 5012;
            written.WriteToValues(current);
            ReceiverSettings loaded = ReceiverSettings::LoadFromValues(current);
            loaded.Sanitize();
            t.Check(loaded.diagnosticsVisible && loaded.firstRunDismissed && loaded.avOffsetMs == 100 &&
                        loaded.videoPort == 5010 && loaded.audioPort == 5012 &&
                        unbox_value_or<int32_t>(current.TryLookup(L"settingsVersion"), 0) == ReceiverSettings::kCurrentSettingsVersion,
                    L"settings: current schema round trip");

            IPropertySet legacy = MakeSettingsMap();
            legacy.Insert(L"videoPort", box_value(5001));
            legacy.Insert(L"avOffsetMs", box_value(100));
            legacy.Insert(L"diagnosticsVisible", box_value(true));
            legacy.Insert(L"acceptRecoveryPoint", box_value(true));
            ReceiverSettings::MigrateStoredSettings(legacy);
            ReceiverSettings legacyLoaded = ReceiverSettings::LoadFromValues(legacy);
            legacyLoaded.Sanitize();
            t.Check(legacyLoaded.avOffsetMs == 100 && legacyLoaded.diagnosticsVisible && legacyLoaded.videoPort == 5001 &&
                        !legacy.HasKey(L"acceptRecoveryPoint") &&
                        unbox_value_or<int32_t>(legacy.TryLookup(L"settingsVersion"), 0) == ReceiverSettings::kCurrentSettingsVersion,
                    L"settings: unversioned migration preserves values and removes obsolete keys");

            IPropertySet legacyDiagDefault = MakeSettingsMap();
            legacyDiagDefault.Insert(L"videoPort", box_value(5000));
            ReceiverSettings::MigrateStoredSettings(legacyDiagDefault);
            ReceiverSettings legacyDiagLoaded = ReceiverSettings::LoadFromValues(legacyDiagDefault);
            legacyDiagLoaded.Sanitize();
            t.Check(legacyDiagLoaded.diagnosticsVisible, L"settings: legacy install without diagnostics key keeps prior default");

            IPropertySet invalid = MakeSettingsMap();
            invalid.Insert(L"settingsVersion", box_value(ReceiverSettings::kCurrentSettingsVersion));
            invalid.Insert(L"avOffsetMs", box_value(9000));
            invalid.Insert(L"videoPort", box_value(80));
            ReceiverSettings invalidLoaded = ReceiverSettings::LoadFromValues(invalid);
            invalidLoaded.Sanitize();
            t.Check(invalidLoaded.avOffsetMs == 500 && invalidLoaded.videoPort == 5000,
                    L"settings: invalid values are clamped on load");

            IPropertySet repeat = MakeSettingsMap();
            repeat.Insert(L"timestampMode", box_value(1));
            ReceiverSettings::MigrateStoredSettings(repeat);
            ReceiverSettings::MigrateStoredSettings(repeat);
            t.Check(!repeat.HasKey(L"timestampMode") &&
                        unbox_value_or<int32_t>(repeat.TryLookup(L"settingsVersion"), 0) == ReceiverSettings::kCurrentSettingsVersion,
                    L"settings: migration is idempotent");

            IPropertySet future = MakeSettingsMap();
            future.Insert(L"settingsVersion", box_value(ReceiverSettings::kCurrentSettingsVersion + 7));
            future.Insert(L"avOffsetMs", box_value(-40));
            future.Insert(L"experimentalFeature", box_value(true));
            ReceiverSettings::MigrateStoredSettings(future);
            ReceiverSettings futureLoaded = ReceiverSettings::LoadFromValues(future);
            futureLoaded.Sanitize();
            t.Check(futureLoaded.avOffsetMs == -40 && future.HasKey(L"experimentalFeature") &&
                        unbox_value_or<int32_t>(future.TryLookup(L"settingsVersion"), 0) ==
                            ReceiverSettings::kCurrentSettingsVersion + 7,
                    L"settings: unknown future schema version is preserved");

            IPropertySet diagOn = MakeSettingsMap();
            diagOn.Insert(L"settingsVersion", box_value(ReceiverSettings::kCurrentSettingsVersion));
            diagOn.Insert(L"diagnosticsVisible", box_value(true));
            ReceiverSettings diagLoaded = ReceiverSettings::LoadFromValues(diagOn);
            diagLoaded.Sanitize();
            t.Check(diagLoaded.diagnosticsVisible, L"settings: diagnostics preference preserved");

            IPropertySet offset = MakeSettingsMap();
            offset.Insert(L"settingsVersion", box_value(ReceiverSettings::kCurrentSettingsVersion));
            offset.Insert(L"avOffsetMs", box_value(100));
            ReceiverSettings offsetLoaded = ReceiverSettings::LoadFromValues(offset);
            offsetLoaded.Sanitize();
            t.Check(offsetLoaded.avOffsetMs == 100, L"settings: manual A/V offset preserved");
        }

        void TestLifecycle(TestContext& t)
        {
            LifecycleGeneration generation;
            uint64_t const start = generation.Advance();
            bool const currentWhileRunning = generation.IsCurrent(start);
            generation.Advance();
            bool const staleAfterStop = !generation.IsCurrent(start);
            uint64_t const restart = generation.Advance();
            t.Check(currentWhileRunning && staleAfterStop && restart != start && generation.IsCurrent(restart) && !generation.IsCurrent(start),
                    L"lifecycle: Stop and a replacement Start invalidate an earlier Start");

            OnceFlag failed;
            bool const first = failed.Set();
            bool const repeated = failed.Set();
            t.Check(first && !repeated && failed.IsSet(), L"lifecycle: a failure is reported once");
        }

        void TestHostileInput(TestContext& t)
        {
            RtpPacketView view;

            bool truncated = true;
            for (size_t n = 0; n < kRtpHeaderSize; ++n)
            {
                Bytes bytes(n, 0x80);
                truncated = truncated && ParseRtpPacket(bytes.data(), bytes.size(), view) == RtpParseResult::TooShort;
            }
            t.Check(truncated, L"hostile rtp: every fixed-header truncation is rejected");

            Bytes csrcShort = MakeRtp(1, 0, 1, false, { 1, 2, 3 });
            csrcShort[0] = static_cast<uint8_t>(0x80 | 15);
            t.Check(ParseRtpPacket(csrcShort.data(), csrcShort.size(), view) == RtpParseResult::BadCsrc,
                    L"hostile rtp: CSRC count larger than the datagram");

            Bytes extHeader = MakeRtp(1, 0, 1, false, { 0xBE, 0xDE });
            extHeader[0] |= 0x10;
            t.Check(ParseRtpPacket(extHeader.data(), extHeader.size(), view) == RtpParseResult::BadExtension,
                    L"hostile rtp: truncated extension header");

            Bytes extFit = MakeRtp(1, 0, 1, false, { 0xBE, 0xDE, 0x00, 0x01, 1, 2, 3, 4, 0x55 });
            extFit[0] |= 0x10;
            Bytes extPast = extFit;
            extPast[15] = 2;
            t.Check(ParseRtpPacket(extFit.data(), extFit.size(), view) == RtpParseResult::Ok && view.payloadSize == 1 &&
                        ParseRtpPacket(extPast.data(), extPast.size(), view) == RtpParseResult::BadExtension,
                    L"hostile rtp: extension length at and past the datagram");

            Bytes padOk = MakeRtp(1, 0, 1, false, { 9, 9, 0, 2 });
            padOk[0] |= 0x20;
            Bytes padZero = MakeRtp(1, 0, 1, false, { 9, 9, 0 });
            padZero[0] |= 0x20;
            Bytes padHuge = MakeRtp(1, 0, 1, false, { 9, 9, 50 });
            padHuge[0] |= 0x20;
            t.Check(ParseRtpPacket(padOk.data(), padOk.size(), view) == RtpParseResult::Ok && view.payloadSize == 2 &&
                        ParseRtpPacket(padZero.data(), padZero.size(), view) == RtpParseResult::BadPadding &&
                        ParseRtpPacket(padHuge.data(), padHuge.size(), view) == RtpParseResult::BadPadding,
                    L"hostile rtp: valid padding, zero padding and padding past the payload");

            RtpStreamTracker tracker;
            RtpAdmissionRules rules;
            auto const now = Clock::now();
            Bytes media(8, 0x11);
            t.Check(AdmitRtpPacket(MakeRtp(1, 0, 1, false, media, 97).data(), 12 + media.size(), rules, tracker, now, view) ==
                        RtpAdmission::WrongPayloadType,
                    L"hostile rtp: wrong payload type");
            Bytes empty = MakeRtp(1, 0, 1, false, {});
            t.Check(ParseRtpPacket(empty.data(), empty.size(), view) == RtpParseResult::EmptyPayload, L"hostile rtp: empty payload");
            Bytes huge = MakeRtp(1, 0, 1, false, Bytes(3000, 1));
            t.Check(AdmitRtpPacket(huge.data(), huge.size(), rules, tracker, now, view) == RtpAdmission::TooLarge && !tracker.Locked(),
                    L"hostile rtp: oversized datagram does not lock a stream");

            SequenceUnwrapper seq;
            TimestampUnwrapper ts;
            int64_t const previousSequence = seq.Unwrap(65535);
            int64_t const wrappedSequence = seq.Unwrap(0);
            int64_t const previousTimestamp = ts.Unwrap(0xFFFFFFF0u);
            int64_t const wrappedTimestamp = ts.Unwrap(0x10u);
            t.Check(wrappedSequence == previousSequence + 1 && wrappedTimestamp > previousTimestamp, L"hostile rtp: sequence and timestamp wrap");

            RtpStreamTracker churn;
            RtpPacketView a;
            a.ssrc = 1;
            a.sequence = 1;
            churn.Check(a, now, std::chrono::seconds(1));
            bool ignored = true;
            for (uint32_t ssrc = 2; ssrc < 20; ++ssrc)
            {
                RtpPacketView foreign = a;
                foreign.ssrc = ssrc;
                foreign.sequence = 1;
                ignored = ignored && churn.Check(foreign, now, std::chrono::seconds(1)) == TrackDecision::Ignore;
            }
            t.Check(ignored && churn.Ssrc() == 1, L"hostile rtp: rapid SSRC alternation does not take over");

            {
                DepackHarness h;
                h.Send(1, 1000, true, {});
                h.Send(2, 2000, true, Bytes{ 0x80 | 1, 0x00 });
                h.Send(3, 3000, true, kPSlice);
                t.Check(h.Pattern() == L"IIC", L"hostile h264: empty and forbidden NAL rejected, valid single NAL kept");
            }
            {
                DepackHarness h;
                h.Send(1, 1000, true, Bytes{ h264::kNalFuA });
                h.Send(2, 2000, true, FuA(0x65, false, true, { 0x88 }));
                h.Send(3, 3000, false, FuA(0x65, true, false, { 0x88 }));
                h.Send(4, 3000, true, FuA(0x65, true, false, { 0x11 }));
                h.Send(5, 4000, true, FuA(0x65, true, true, { 0x88 }));
                t.Check(h.depack.GetCounters().fuaErrors >= 4, L"hostile h264: missing FU header, continuation, repeated start, start+end");
            }
            {
                H264Depacketizer::Limits limits;
                limits.maxFuFragments = 4;
                DepackHarness h;
                h.depack = H264Depacketizer([&h](AccessUnitPtr au) { h.units.push_back(std::move(au)); }, limits);
                h.Send(1, 1000, false, FuA(0x65, true, false, { 0x80 }));
                h.Send(2, 1000, false, FuA(0x65, false, false, { 0x01 }));
                h.Send(3, 1000, false, FuA(0x65, false, false, { 0x02 }));
                h.Send(4, 1000, false, FuA(0x65, false, false, { 0x03 }));
                h.Send(5, 1000, true, FuA(0x65, false, true, { 0x04 }));
                t.Check(h.Pattern() == L"I" && h.depack.GetCounters().oversize >= 1, L"hostile h264: FU-A fragment cap");
            }
            {
                H264Depacketizer::Limits limits;
                limits.maxAuBytes = 16;
                DepackHarness h;
                h.depack = H264Depacketizer([&h](AccessUnitPtr au) { h.units.push_back(std::move(au)); }, limits);
                h.Send(1, 1000, true, Bytes(64, 0x65));
                t.Check(h.Pattern() == L"I" && h.IncompleteCarryNoData(), L"hostile h264: access-unit byte cap");
            }
            {
                DepackHarness h;
                h.Send(1, 1000, true, Bytes{ 0x78, 0x00, 0x02 });
                h.Send(2, 2000, true, Bytes{ 0x78, 0x00, 0x00, 0x67 });
                h.Send(3, 3000, true, Bytes{ 0x78, 0x00, 0x04, 0x67, 0x42 });
                std::vector<Bytes> many(40, Bytes{ 0x67, 0x42 });
                h.Send(4, 4000, true, StapA(many));
                t.Check(h.Pattern() == L"IIII" && h.depack.GetCounters().stapaErrors == 4,
                        L"hostile h264: truncated, zero-length, overrun and excessive STAP-A");
            }
            {
                DepackHarness h;
                Bytes const sps = MakeSps(false, 120, 68, 4, false);
                h.Send(1, 1000, true, kPSlice);
                h.Send(2, 2000, true, StapA({ sps, kPps }));
                H264KeyframeGate gate;
                auto dropped = h.units.size() < 2 ? H264KeyframeGate::Decision::Submit : gate.Process(*h.units[1]).decision;
                t.Check(h.units.size() >= 2 && h.units[1]->hasSps && !h.units[1]->hasSlice && !h.units[1]->corrupt &&
                            dropped == H264KeyframeGate::Decision::DropNoSlice,
                        L"hostile h264: SPS/PPS-only access unit is not submitted as a picture");
                h.units.clear();
                h.Send(2, 2000, true, kIdrSlice);
                h.Send(3, 3000, false, kPSlice);
                h.Send(5, 4000, true, kPSlice);
                h.Send(6, 5000, false, sps);
                h.Send(7, 5000, false, kPps);
                h.Send(8, 5000, true, kIdrSlice);
                t.Check(h.units.size() >= 2, L"hostile h264: corrupt input then a later access unit still completes");
            }

            SpsInfo info;
            Bytes const baseline = MakeSps(false, 120, 68, 4, false);
            bool truncations = true;
            for (size_t n = 0; n < baseline.size(); n += 3)
            {
                truncations = truncations && !ParseSps(baseline.data(), n < 4 ? n : std::min(n, size_t{ 6 }), info);
            }
            t.Check(truncations && !ParseSps(baseline.data(), 3, info), L"hostile sps: truncated prefixes fail closed");

            Bytes zeros(32, 0);
            zeros[0] = 0x67;
            t.Check(!ParseSps(zeros.data(), zeros.size(), info), L"hostile sps: pathological Exp-Golomb zeros fail closed");

            Bytes hugeSps = MakeSps(false, 2000, 2000, 0, false);
            t.Check(!ParseSps(hugeSps.data(), hugeSps.size(), info), L"hostile sps: dimensions above the safe range are rejected");

            BitWriter crop;
            crop.Bits(66, 8);
            crop.Bits(0, 8);
            crop.Bits(40, 8);
            crop.Ue(0); // sps id
            crop.Ue(0); // log2_max_frame_num_minus4
            crop.Ue(0); // pic_order_cnt_type
            crop.Ue(0); // log2_max_pic_order_cnt_lsb_minus4
            crop.Ue(1); // max_num_ref_frames
            crop.Bit(0);
            crop.Ue(15);
            crop.Ue(15);
            crop.Bit(1);
            crop.Bit(1);
            crop.Bit(1);
            crop.Ue(0xFFFFFFFEu);
            crop.Ue(2);
            crop.Ue(0);
            crop.Ue(0);
            crop.Bit(0);
            Bytes cropNal{ 0x67 };
            Bytes cropBody = AddEmulationPrevention(crop.Finish());
            cropNal.insert(cropNal.end(), cropBody.begin(), cropBody.end());
            t.Check(!ParseSps(cropNal.data(), cropNal.size(), info), L"hostile sps: overflowing crop offsets are rejected");

            t.Check(ParseSps(baseline.data(), baseline.size(), info) && info.width == 1920 && info.height == 1080,
                    L"hostile sps: a valid SPS still parses after malformed input");

            {
                auto stats = std::make_shared<ReceiverStats>();
                auto pcm = std::make_shared<PcmRingBuffer>(64, 2);
                ReceiverSettings settings;
                AudioReceiver receiver(settings, stats, pcm);
                auto const audioNow = Clock::now();
                Bytes odd = MakeRtp(1, 0, 9, false, Bytes(6, 0x10));
                Bytes zero = MakeRtp(2, 0, 9, false, {});
                receiver.ProcessDatagram(odd.data(), odd.size(), audioNow);
                receiver.ProcessDatagram(zero.data(), zero.size(), audioNow);
                t.Check(!receiver.EverReceived() && pcm->Available() == 0, L"hostile audio: odd and empty payloads do not update activity");

                Bytes aligned(2048 - 12, 0x10);
                Bytes maxPacket = MakeRtp(10, 0, 9, false, aligned);
                Bytes over = MakeRtp(11, 1000, 9, false, Bytes(2048, 0x10));
                receiver.ProcessDatagram(maxPacket.data(), maxPacket.size(), audioNow);
                auto const audioLater = audioNow + std::chrono::milliseconds(40);
                receiver.ProcessDatagram(over.data(), over.size(), audioLater);
                t.Check(receiver.EverReceived() && receiver.LastPacketTime() == audioNow && pcm->Available() > 0,
                        L"hostile audio: maximum aligned payload is accepted and the next oversized packet is not");

                Bytes wrapped = MakeRtp(11, 0x10u, 9, false, Bytes(8, 0x20));
                receiver.ProcessDatagram(wrapped.data(), wrapped.size(), audioLater);
                t.Check(receiver.LastPacketTime() == audioLater, L"hostile audio: a later valid packet is accepted after junk");
            }

            {
                PcmRingBuffer ring(4, 2);
                float in[16] = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8 };
                float out[8] = {};
                t.Check(ring.Write(in, 6) == 4 && ring.Read(out, 4) == 4 && ring.Available() == 0 && ring.Write(in, 2) == 2,
                        L"hostile audio: ring full and empty transitions stay inside capacity");
            }

            auto stats = std::make_shared<ReceiverStats>();
            stats->Set(Stat::VideoInvalid, INT64_MAX - 2);
            stats->Add(Stat::VideoInvalid, 10);
            t.Check(stats->Get(Stat::VideoInvalid) == INT64_MAX, L"hostile stats: counters saturate instead of wrapping");

            uint32_t seed = 0xC0FFEEu;
            auto next = [&]()
            {
                seed = seed * 1664525u + 1013904223u;
                return seed;
            };
            bool survived = true;
            for (int i = 0; i < 400; ++i)
            {
                size_t const n = next() % 48;
                Bytes blob(n);
                for (size_t b = 0; b < n; ++b)
                {
                    blob[b] = static_cast<uint8_t>(next());
                }
                RtpPacketView parsed;
                ParseRtpPacket(blob.empty() ? nullptr : blob.data(), blob.size(), parsed);
                SpsInfo sps;
                if (!blob.empty())
                {
                    ParseSps(blob.data(), blob.size(), sps);
                }
                DepackHarness h;
                h.Send(static_cast<int64_t>(i + 1), static_cast<int64_t>(next()), (next() & 1) != 0, blob);
                survived = survived && h.units.size() <= 1;
                if (!survived)
                {
                    Log(L"SELFTEST FAIL: hostile stress seed=0x%X iteration=%d", 0xC0FFEEu, i);
                    break;
                }
            }
            t.Check(survived, L"hostile stress: seeded mutated buffers stay bounded");
        }

        void TestHealth(TestContext& t)
        {
            RollingSampleWindow<4> window;
            int64_t value = 0;
            t.Check(!window.Percentile(50, value) && window.Count() == 0, L"health window: empty has no percentile");
            window.Push(10);
            t.Check(window.Percentile(50, value) && value == 10, L"health window: single sample");
            window.Push(1);
            window.Push(3);
            window.Push(2);
            t.Check(window.Count() == 4 && window.Percentile(50, value) && value == 3, L"health window: median of four");
            int64_t p95 = 0;
            t.Check(window.Percentile(95, p95) && p95 == 10, L"health window: p95 picks the high end");
            window.Push(7);
            t.Check(window.Count() == 4, L"health window: insertion evicts the oldest");
            t.Check(window.Percentile(0, value) && value == 1, L"health window: minimum after eviction");

            RollingSampleWindow<5> duplicates;
            for (int i = 0; i < 5; ++i)
            {
                duplicates.Push(4);
            }
            duplicates.Push(100);
            t.Check(duplicates.Percentile(50, value) && value == 4, L"health window: duplicate values stay at the median");
            t.Check(duplicates.Percentile(100, value) && value == 100, L"health window: outlier is the maximum percentile");

            double rate = 0;
            t.Check(EventsPerSecond(100, 1000000, rate) && rate == 100.0, L"health rate: events per second");
            t.Check(!EventsPerSecond(10, 0, rate) && !EventsPerSecond(10, -1, rate), L"health rate: zero or backward elapsed time is invalid");

            auto const t0 = Clock::time_point(std::chrono::milliseconds(1000));
            int64_t age = 0;
            t.Check(AgeMs(false, t0, t0, age) == SampleAge::Never && age == -1, L"health age: never");
            t.Check(AgeMs(true, t0, t0 + std::chrono::milliseconds(40), age) == SampleAge::Ok && age == 40, L"health age: elapsed");
            t.Check(AgeMs(true, t0, t0 - std::chrono::milliseconds(1), age) == SampleAge::Invalid && age == -1, L"health age: backward clock");

            HighWater water;
            water.Observe(2);
            water.Observe(9);
            water.Observe(4);
            t.Check(water.Current() == 4 && water.Peak() == 9, L"health high water: current and peak");
            water.Observe(-1);
            t.Check(water.Invalid() && water.Peak() == 9, L"health high water: negative sample does not lower the peak");
            water.Reset();
            t.Check(water.Current() == 0 && water.Peak() == 0, L"health high water: reset");

            DurationWatch stall;
            auto const s0 = Clock::time_point(std::chrono::seconds(10));
            stall.SetActive(true, s0);
            stall.SetActive(true, s0 + std::chrono::milliseconds(1500));
            t.Check(stall.Active() && stall.CurrentMs() == 1500 && stall.Starts() == 1, L"health stall: continuation");
            stall.SetActive(false, s0 + std::chrono::milliseconds(1600));
            stall.SetActive(true, s0 + std::chrono::milliseconds(3000));
            stall.SetActive(true, s0 + std::chrono::milliseconds(4500));
            t.Check(stall.Starts() == 2 && stall.LongestMs() == 1500 && stall.CurrentMs() == 1500, L"health stall: longest is kept after recovery");
            stall.Reset();
            t.Check(!stall.Active() && stall.LongestMs() == 0 && stall.Starts() == 0, L"health stall: reset");

            DurationWatch idr;
            idr.SetActive(true, s0);
            idr.SetActive(true, s0 + std::chrono::milliseconds(2500));
            idr.SetActive(false, s0 + std::chrono::milliseconds(2600));
            t.Check(!idr.Active() && idr.LongestMs() == 2500 && idr.CurrentMs() == 0, L"health idr wait: completion keeps the longest");

            t.Check(ClassifyAudioBuffer(-1, 50) == AudioBufferClass::Unknown, L"health audio: unknown without a fill");
            t.Check(ClassifyAudioBuffer(24999, 50) == AudioBufferClass::Low, L"health audio: below half the target");
            t.Check(ClassifyAudioBuffer(25000, 50) == AudioBufferClass::InRange, L"health audio: half the target is in range");
            t.Check(ClassifyAudioBuffer(100000, 50) == AudioBufferClass::InRange, L"health audio: twice the target is in range");
            t.Check(ClassifyAudioBuffer(100001, 50) == AudioBufferClass::High, L"health audio: above twice the target");

            auto const origin = Clock::time_point(std::chrono::seconds(0));
            SessionHealth health;
            health.Reset(origin);
            HealthObservation obs;
            obs.receiving = true;
            obs.audioEnabled = true;
            obs.videoEver = true;
            obs.audioEver = true;
            obs.videoLast = origin;
            obs.audioLast = origin;
            obs.submitEver = true;
            obs.submitLast = origin;
            obs.queueFrames = 8;
            obs.queueFramesHigh = 8;
            health.Update(obs, origin);
            health.Update(obs, origin + std::chrono::milliseconds(1999));
            t.Check((health.Flags() & HealthQueueSustained) == 0, L"health queue: just under the sustained threshold");
            health.Update(obs, origin + std::chrono::milliseconds(2000));
            t.Check((health.Flags() & HealthQueueSustained) != 0, L"health queue: sustained high queue");

            health.Reset(origin);
            obs.queueFrames = 1;
            obs.queueFramesHigh = 1;
            obs.videoLast = origin + std::chrono::milliseconds(5000);
            obs.submitEver = true;
            obs.submitLast = origin;
            obs.waitingForIdr = false;
            health.Update(obs, origin + std::chrono::milliseconds(5200));
            t.Check((health.Flags() & HealthPacketsWithoutSubmit) != 0, L"health: packets without a submitted sample");

            health.Reset(origin);
            obs.waitingForIdr = true;
            health.Update(obs, origin + std::chrono::milliseconds(5200));
            t.Check((health.Flags() & HealthPacketsWithoutSubmit) == 0, L"health: keyframe wait is not a missing submit");

            health.Reset(origin);
            obs = {};
            obs.receiving = true;
            obs.audioEnabled = true;
            obs.videoEver = true;
            obs.audioEver = true;
            obs.videoLast = origin;
            obs.audioLast = origin;
            health.Update(obs, origin + std::chrono::milliseconds(1000));
            t.Check((health.Flags() & HealthStall) != 0 && health.Stall().Starts() == 1, L"health stall: both directions quiet");
            obs.audioLast = origin + std::chrono::milliseconds(1000);
            health.Update(obs, origin + std::chrono::milliseconds(1000));
            t.Check((health.Flags() & HealthStall) == 0 && health.Stall().LongestMs() >= 0, L"health stall: audio clears a static-screen false stall");

            health.Reset(origin);
            obs = {};
            obs.sourceBuilds = 1;
            health.Update(obs, origin);
            obs.sourceBuilds = 2;
            health.Update(obs, origin + std::chrono::milliseconds(100));
            obs.sourceBuilds = 3;
            health.Update(obs, origin + std::chrono::milliseconds(200));
            t.Check((health.Flags() & HealthRepeatedRestarts) != 0, L"health: three source builds inside the window");

            health.Reset(origin);
            obs = {};
            obs.idrWaitMs = 2000;
            health.Update(obs, origin);
            t.Check((health.Flags() & HealthIdrWaitExtended) != 0, L"health: extended IDR wait");

            health.Reset(origin);
            obs = {};
            obs.queueFramesHigh = 9;
            health.Update(obs, origin);
            obs.queueFramesHigh = 10;
            health.Update(obs, origin + std::chrono::milliseconds(10));
            t.Check((health.Flags() & HealthQueuePeakRising) != 0, L"health: queue peak still rising");

            auto stats = std::make_shared<ReceiverStats>();
            stats->Add(Stat::VideoAccepted, INT64_MAX);
            t.Check(stats->Get(Stat::VideoAccepted) == INT64_MAX, L"health stats: accepted counter saturates");
            stats->Raise(Stat::FrameQueueFramesHigh, 3);
            stats->Raise(Stat::FrameQueueFramesHigh, 1);
            t.Check(stats->Get(Stat::FrameQueueFramesHigh) == 3, L"health stats: high water does not fall");

            wchar_t label[32] = {};
            FormatHealth(HealthOk, label, 32);
            t.Check(std::wcscmp(label, L"ok") == 0, L"health label: ok");
            FormatHealth(HealthStall, label, 32);
            t.Check(std::wcscmp(label, L"stall") == 0, L"health label: stall");

            SessionHealth simulated;
            auto cursor = origin;
            simulated.Reset(cursor);
            HealthObservation steady;
            steady.receiving = true;
            steady.audioEnabled = true;
            steady.videoEver = true;
            steady.audioEver = true;
            steady.submitEver = true;
            steady.audioFillUs = 50000;
            steady.audioTargetMs = 50;
            steady.queueFrames = 2;
            steady.queueFramesHigh = 2;
            for (int i = 0; i < 200000; ++i)
            {
                cursor += std::chrono::milliseconds(10);
                steady.videoLast = cursor;
                steady.audioLast = cursor;
                steady.submitLast = cursor;
                simulated.Update(steady, cursor);
            }
            t.Check(simulated.UptimeMs(cursor) == 2000000 && simulated.Flags() == HealthOk, L"health: injected long session stays healthy");
            t.Check(sizeof(RollingSampleWindow<128>) < 4096, L"health window: fixed memory");
        }

        void TestUiChrome(TestContext& t)
        {
            t.Check(SanitizeOverlay(0) == PlaybackOverlay::Automatic && SanitizeOverlay(1) == PlaybackOverlay::AlwaysVisible &&
                        SanitizeOverlay(2) == PlaybackOverlay::VideoOnly && SanitizeOverlay(99) == PlaybackOverlay::Automatic,
                    L"ui overlay: invalid values fall back to Automatic");
            t.Check(std::wcscmp(AppDisplayName(), L"XReceiver") == 0 && wcsstr(AppDisplayName(), L"Smoke") == nullptr,
                    L"ui title is XReceiver");

            ChromeInput healthy;
            healthy.overlay = PlaybackOverlay::VideoOnly;
            healthy.state = ChromeState::Receiving;
            healthy.usableVideo = true;
            ChromeVisibility clean = DecideChrome(healthy);
            t.Check(clean.videoClean && !clean.status && !clean.controls && !clean.diagnostics && !clean.waitingGuide,
                    L"ui video only: healthy playback is unobstructed");

            ChromeInput menu = ApplyChromeAction(healthy, ChromeAction::Menu);
            ChromeVisibility withPanel = DecideChrome(menu);
            t.Check(withPanel.controls && !withPanel.status && !withPanel.videoClean, L"ui video only: Menu opens the panel only");
            ChromeInput closed = ApplyChromeAction(menu, ChromeAction::Back);
            ChromeVisibility afterBack = DecideChrome(closed);
            t.Check(afterBack.videoClean && !afterBack.controls && !afterBack.diagnostics, L"ui video only: B returns to clean playback");
            t.Check(closed.state == ChromeState::Receiving && closed.usableVideo, L"ui video only: B does not change media state");

            ChromeInput view = ApplyChromeAction(healthy, ChromeAction::View);
            t.Check(DecideChrome(view).diagnostics && !DecideChrome(view).status, L"ui video only: View shows diagnostics only");

            ChromeInput reconnect = healthy;
            reconnect.state = ChromeState::Reconnecting;
            reconnect.usableVideo = false;
            t.Check(DecideChrome(reconnect).status && !DecideChrome(reconnect).videoClean, L"ui video only: reconnecting restores status");

            ChromeInput automatic = healthy;
            automatic.overlay = PlaybackOverlay::Automatic;
            automatic.statusHold = true;
            t.Check(DecideChrome(automatic).status, L"ui automatic: status holds after a change");
            automatic.statusHold = false;
            t.Check(!DecideChrome(automatic).status, L"ui automatic: status hides during steady playback");

            ChromeInput always = healthy;
            always.overlay = PlaybackOverlay::AlwaysVisible;
            always.statusHold = false;
            t.Check(DecideChrome(always).status && !DecideChrome(always).controls, L"ui always visible: status stays and controls can hide");

            ChromeInput waiting;
            waiting.state = ChromeState::Waiting;
            waiting.overlay = PlaybackOverlay::Automatic;
            t.Check(DecideChrome(waiting).status && DecideChrome(waiting).waitingGuide, L"ui waiting: guide is shown");

            StatusCopy key = StatusFor(ChromeState::Receiving, true, true, false);
            StatusCopy stopped = StatusFor(ChromeState::Stopped, false, true, false);
            t.Check(std::wcscmp(key.title, L"Waiting for video keyframe") == 0 && std::wcscmp(stopped.title, L"Receiver stopped") == 0,
                    L"ui status: keyframe and stopped copy");

            wchar_t offset[64] = {};
            FormatAvOffset(100, offset, 64);
            t.Check(std::wcscmp(offset, L"+100 ms, audio later") == 0, L"ui offset: positive means audio later");
            FormatAvOffset(-40, offset, 64);
            t.Check(std::wcscmp(offset, L"-40 ms, audio earlier") == 0, L"ui offset: negative means audio earlier");

            wchar_t missing[16] = {};
            FormatUnavailable(false, 0, L" KB", missing, 16);
            t.Check(std::wcscmp(missing, L"\u2014") == 0, L"ui metrics: unavailable is an em dash");

            FocusTarget hidden = FocusForChrome(healthy, clean, true);
            t.Check(hidden == FocusTarget::None, L"ui focus: hidden chrome keeps no control focus");
            ChromeInput panel = healthy;
            panel.panelOpen = true;
            t.Check(FocusForChrome(panel, DecideChrome(panel), true) == FocusTarget::Stop, L"ui focus: open panel focuses Stop while receiving");

            ControlAvailability controls = ControlsFor(true, false);
            t.Check(!controls.ports && controls.stop && controls.overlay && controls.avOffset, L"ui controls: ports lock while receiving, overlay stays available");
            t.Check(!ShowDeveloperControls(false) && ShowDeveloperControls(true), L"ui: developer controls follow the build");

            IPropertySet legacy = MakeSettingsMap();
            legacy.Insert(L"settingsVersion", box_value(2));
            legacy.Insert(L"avOffsetMs", box_value(100));
            legacy.Insert(L"firstRunDismissed", box_value(true));
            ReceiverSettings::MigrateStoredSettings(legacy);
            ReceiverSettings migrated = ReceiverSettings::LoadFromValues(legacy);
            migrated.Sanitize();
            t.Check(migrated.playbackOverlay == PlaybackOverlay::Automatic && migrated.avOffsetMs == 100 && migrated.firstRunDismissed &&
                        unbox_value_or<int32_t>(legacy.TryLookup(L"settingsVersion"), 0) == ReceiverSettings::kCurrentSettingsVersion,
                    L"ui settings: existing users migrate to Automatic without onboarding");

            ReceiverSettings junk;
            junk.playbackOverlay = static_cast<PlaybackOverlay>(40);
            junk.avOffsetMs = 100;
            junk.Sanitize();
            t.Check(junk.playbackOverlay == PlaybackOverlay::Automatic && junk.avOffsetMs == 100, L"ui settings: invalid overlay falls back and keeps the offset");
        }
    }

    SelfTestResult RunSelfTests()
    {
        TestContext t;
        TestRtpParser(t);
        TestUnwrappers(t);
        TestJitterBuffer(t);
        TestTracker(t);
        TestRtpAdmission(t);
        TestDepacketizer(t);
        TestVideoPipeline(t);
        TestSps(t);
        TestGate(t);
        TestVideoRecovery(t);
        TestVideoTimeline(t);
        TestFrameDelivery(t);
        TestVideoDeliveryCore(t);
        TestFormatRecovery(t);
        TestNewStreamHandover(t);
        TestDeliveryConfig(t);
        TestVideoIntegration(t);
        TestAudio(t);
        TestAudioActivity(t);
        TestAudioTimestampReset(t);
        TestResampler(t);
        TestAudioStage(t);
        TestManualAvOffset(t);
        TestLifecycle(t);
        TestReceiverSettings(t);
        TestHostileInput(t);
        TestHealth(t);
        TestUiChrome(t);

        SelfTestResult result;
        result.passed = t.passed;
        result.failed = t.failed;
        std::wstring summary = L"Self-test: " + std::to_wstring(t.passed) + L" passed, " + std::to_wstring(t.failed) + L" failed";
        summary += t.failures;
        result.summary = winrt::hstring(summary);
        Log(L"%s", summary.c_str());
        return result;
    }
}
