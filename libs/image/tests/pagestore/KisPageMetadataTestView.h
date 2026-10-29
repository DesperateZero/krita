/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_METADATA_TEST_VIEW_H
#define KIS_PAGE_METADATA_TEST_VIEW_H
#include "KisPageViewTestAccess.h"
#include <algorithm>
#include <memory>

// Independent visibility input for standalone metadata tests. Selection comes
// from fixture inputs, never from metadata's stored phases. Composed owner
// tests use their real epoch model instead. This fixture is not a publisher:
// the test explicitly advances it only after a successful metadata install.
class KisPageMetadataTestView
{
public:
    KisPageMetadataTestView()
    {
        expected.epoch = {1};
        expected.graphRevision = expected.defaultPixelRevision = expected.extentRevision = expected.propertyRevision = 1;
    }
    bool registerPage(KisPageMetadataCoordinator &metadata, const KisPageStateSnapshot &page, QString *error = nullptr)
    {
        if (!KisPageStateMachine().validateInvariants(page)) return metadata.registerPage(page, error);
        const auto previous = expected;
        select({page.key, page.publishedGeneration, page.publishedDefaultPixelRevision});
        expected.epoch.value = std::max(expected.epoch.value, page.publishedEpoch.value);
        // Match actual Store initialization: the view holds the initial
        // identity before registration fills that same Value's content.
        auto next = std::make_unique<KisImageEpochReferenceModel>();
        if (!KisPageViewTestAccess::initialize(*next, metadata, expected, error)) {
            expected = previous;
            return false;
        }
        if (!metadata.registerPage(page, error)) {
            expected = previous;
            return false;
        }
        epochs = std::move(next);
        return true;
    }
    bool accept(KisPageMetadataCoordinator &metadata, KisImageEpochId epoch, const KisPageVersion &version)
    {
        select(version, true); expected.epoch = epoch;
        return rebuild(metadata, nullptr);
    }
    template<class Changes>
    bool accept(KisPageMetadataCoordinator &metadata, KisImageEpochId epoch, const Changes &changes)
    {
        for (const auto &change : changes) select(change.version, true);
        expected.epoch = epoch;
        return rebuild(metadata, nullptr);
    }
    void reset() { epochs.reset(); expected.manifest.clear(); expected.surfaces.clear(); }
    KisImageEpochReferenceModel &model() { Q_ASSERT(epochs); return *epochs; }
    KisImageEpochRootSnapshot root() const
    {
        if (epochs) return epochs->captureCommittedRoot();
        // Empty publication is still bound to a real (empty) source view.
        KisImageEpochReferenceModel empty;
        if (!empty.initialize(expected)) return {};
        return empty.captureCommittedRoot();
    }
private:
    void select(const KisPageVersion &version, bool accepted = false)
    {
        if (accepted && version.isDefaultPixel() && version.generation.value == 1) {
            // A view cut selects the surface default; it does not import a
            // new explicit metadata Value. Build this selection from fixture
            // input, independently of whether a physical cache exists.
            expected.manifest.erase(std::remove_if(expected.manifest.begin(), expected.manifest.end(),
                [&](const auto &old) { return old.key == version.key; }), expected.manifest.end());
            auto surface = std::find_if(expected.surfaces.begin(), expected.surfaces.end(),
                [&](const auto &s) { return s.surface == version.key.surface; });
            if (surface == expected.surfaces.end()) {
                KisSurfaceEpochState state;
                state.surface = version.key.surface;
                state.logicalPageExtent = QSize(64, 64);
                state.layoutRevision = state.extentRevision = 1;
                state.format.formatId = 1;
                state.format.colorModelId = "ALPHA"; state.format.colorDepthId = "U8";
                state.format.profileFingerprint = "metadata-view-fixture";
                state.format.channelOrder = "A"; state.format.packing = "interleaved";
                state.format.defaultPixel = QByteArray(1, 0);
                state.format.channelCount = state.format.pixelStride = state.format.pixelAlignment = 1;
                state.format.alphaSemantic = KisSurfaceAlphaSemantic::SelectionMask;
                state.format.endianness = KisSurfaceEndianness::NativeEndian;
                state.format.codecVersion = 1;
                expected.surfaces.append(state);
                surface = expected.surfaces.end() - 1;
            }
            surface->defaultPixelRevision = version.defaultPixelRevision;
            return;
        }
        const auto it = std::find_if(expected.manifest.begin(), expected.manifest.end(),
            [&](const auto &old) { return old.key == version.key; });
        if (it == expected.manifest.end()) expected.manifest.append(version);
        else *it = version;
    }
    bool rebuild(KisPageMetadataCoordinator &metadata, QString *error)
    {
        auto next = std::make_unique<KisImageEpochReferenceModel>();
        if (!KisPageViewTestAccess::initialize(*next, metadata, expected, error)) return false;
        epochs = std::move(next);
        return true;
    }
    KisImageEpochSnapshot expected;
    std::unique_ptr<KisImageEpochReferenceModel> epochs;
};
#endif
