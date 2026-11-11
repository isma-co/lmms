#include <QtTest>

#include "AudioDevice.h"
#include "Engine.h"
#include "PlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"
#include "Song.h"

using namespace lmms;

class SampleClipTrimTest : public QObject
{
	Q_OBJECT

	static void loadSample(SampleClip& clip)
	{
		// 200.5 ticks at 120 BPM. Deliberately differ from the engine's sample rate.
		clip.setSampleBuffer(std::make_shared<SampleBuffer>(
			std::vector<SampleFrame>(100250, SampleFrame{0.25f, 0.25f}), 48000));
		clip.movePosition(100);
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); // Render explicitly for deterministic playback checks.
		Engine::getSong()->setTempo(120);
	}

	void cleanupTestCase() { Engine::destroy(); }

	void init() { Engine::projectJournal()->clearJournal(); }

	void leftRightAndSourceBounds()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		QCOMPARE(clip.sampleLength().getTicks(), 200);
		QVERIFY(clip.trim(150, 150));
		QCOMPARE(clip.startPosition().getTicks(), 150);
		QCOMPARE(clip.endPosition().getTicks(), 300);
		QCOMPARE(clip.startTimeOffset().getTicks(), -50);
		QVERIFY(clip.trim(150, 40));
		QVERIFY(clip.trim(150, 1000));
		QCOMPARE(clip.length().getTicks(), 150);
		QVERIFY(clip.trim(0, 300));
		QCOMPARE(clip.startPosition().getTicks(), 100);
		QCOMPARE(clip.length().getTicks(), 200);
		QCOMPARE(clip.startTimeOffset().getTicks(), 0);
		QCOMPARE(clip.sample().sampleSize(), size_t{100250});
		QVERIFY(!clip.getAutoResize());
	}

	void snappedRequestsStopAtSource()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		QVERIFY(clip.trim(140, 160));
		const auto snappedStart = TimePos(0).quantize(0.25f);
		QVERIFY(clip.trim(snappedStart, clip.endPosition() - snappedStart));
		QCOMPARE(clip.startPosition().getTicks(), 100); // Off-grid source start wins.
		QVERIFY(clip.trim(100, 50));
		const auto snappedEnd = TimePos(340).quantize(0.25f);
		QVERIFY(clip.trim(100, snappedEnd - clip.startPosition()));
		QCOMPARE(clip.endPosition().getTicks(), 300); // Off-grid source end wins.
		QVERIFY(clip.trim(299, 1));
		QVERIFY(!clip.trim(999, 1));
		QCOMPARE(clip.length().getTicks(), 1);
	}

	void splitOffsetsCanBeTrimmedAndRecovered()
	{
		SampleTrack track(Engine::getSong());
		SampleClip left(&track);
		loadSample(left);
		QVERIFY(left.trim(120, 180));
		// Same clone/geometry/offset operations as ClipView::splitClip, including
		// splitting a clip which already has an offset.
		auto right = std::unique_ptr<SampleClip>(left.clone());
		left.changeLength(60);
		left.setAutoResize(false);
		right->movePosition(180);
		right->changeLength(120);
		right->setStartTimeOffset(left.startTimeOffset() - left.length());
		right->setAutoResize(false);
		QVERIFY(right->trim(210, 90));
		QCOMPARE(right->startTimeOffset().getTicks(), -110);
		QVERIFY(right->trim(210, 30));
		QVERIFY(right->trim(210, 1000));
		QCOMPARE(right->endPosition().getTicks(), 300);
		QVERIFY(right->trim(0, 300));
		QCOMPARE(right->startPosition().getTicks(), 100);
		QCOMPARE(right->startTimeOffset().getTicks(), 0);
		QCOMPARE(left.startPosition().getTicks(), 120);
		QCOMPARE(left.length().getTicks(), 60);
	}

	void undoRedoGesture()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		auto journal = Engine::projectJournal();
		journal->clearJournal();
		clip.addJournalCheckPoint();
		clip.saveJournallingState(false);
		QVERIFY(clip.trim(130, 170));
		QVERIFY(clip.trim(150, 150));
		QVERIFY(clip.trim(150, 60));
		clip.restoreJournallingState();
		QVERIFY(clip.isJournalling());
		QVERIFY(clip.isJournallingStateStackEmpty());
		journal->undo();
		QCOMPARE(clip.startPosition().getTicks(), 100);
		QCOMPARE(clip.length().getTicks(), 200);
		QCOMPARE(clip.startTimeOffset().getTicks(), 0);
		QVERIFY(clip.getAutoResize());
		QVERIFY(!journal->canUndo());
		journal->redo();
		QCOMPARE(clip.startPosition().getTicks(), 150);
		QCOMPARE(clip.length().getTicks(), 60);
		QCOMPARE(clip.startTimeOffset().getTicks(), -50);
		QVERIFY(!clip.getAutoResize());
	}

	void saveRestoreAndLegacySilence()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		QVERIFY(clip.trim(150, 150));
		QVERIFY(clip.trim(150, 75));
		QDomDocument doc;
		auto parent = doc.createElement("test");
		doc.appendChild(parent);
		clip.saveState(doc, parent);
		// Exercise the existing XML format through a byte round trip.
		QDomDocument reopened;
		QVERIFY(reopened.setContent(doc.toByteArray()));
		SampleClip restored(&track);
		restored.restoreState(reopened.documentElement().firstChildElement());
		QCOMPARE(restored.startPosition(), clip.startPosition());
		QCOMPARE(restored.length(), clip.length());
		QCOMPARE(restored.startTimeOffset(), clip.startTimeOffset());
		QCOMPARE(restored.sample().sampleSize(), clip.sample().sampleSize());
		QVERIFY(restored.trim(150, 1000));
		QCOMPARE(restored.endPosition().getTicks(), 300);

		auto legacy = reopened.documentElement().firstChildElement();
		legacy.setAttribute("off", 50);
		legacy.setAttribute("len", 350);
		restored.restoreState(legacy);
		QCOMPARE(restored.startTimeOffset().getTicks(), 50);
		QCOMPARE(restored.length().getTicks(), 350); // Loading never clamps.
		QVERIFY(!restored.trim(150, 350));
		QVERIFY(restored.trim(220, 280));
		QCOMPARE(restored.endPosition().getTicks(), 500); // Untouched legacy tail.
	}

	void noOpAndRecording()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		auto song = Engine::getSong();
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QVERIFY(song->guiSaveProjectAs(directory.filePath("trim.mmp")));
		QVERIFY(!clip.trim(100, 200));
		QVERIFY(!song->isModified());
		QVERIFY(!clip.trim(100, 1000));
		QVERIFY(!song->isModified());
		QVERIFY(clip.getAutoResize());
		QVERIFY(clip.trim(150, 150));
		QVERIFY(song->isModified());
		clip.setRecord(true);
		QVERIFY(clip.trim(0, 300)); // Preserve pre-take/record-armed resize behavior.
		QCOMPARE(clip.startPosition().getTicks(), 0);
		QVERIFY(clip.trim(0, 1000));
		SampleClip empty(&track);
		QVERIFY(empty.trim(0, 1000));
	}

	void playbackSeesCompleteGeometry()
	{
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		loadSample(clip);
		auto engine = Engine::audioEngine();
		QVERIFY(track.play(100, engine->framesPerPeriod(), 0));
		engine->renderNextPeriod(); // Promote the scheduled handle to active playback.
		QVERIFY(clip.isPlaying());
		bool complete = false;
		connect(&clip, &Clip::positionChanged, this, [&]
		{
			complete = clip.startPosition() == 150 && clip.length() == 150
				&& clip.startTimeOffset() == -50;
		});
		QVERIFY(clip.trim(150, 150));
		QVERIFY(complete);
		QVERIFY(!clip.isPlaying());
		for (auto handle : engine->playHandles()) { QVERIFY(!handle->isFromTrack(&track)); }
		QVERIFY(track.play(160, engine->framesPerPeriod(), 0));
		QCOMPARE(clip.sample().startFrame(), 30000); // 60 ticks into source at 48 kHz.
		QCOMPARE(clip.sample().endFrame(), 100000);
		engine->renderNextPeriod();
		QVERIFY(clip.trim(150, 50)); // Right trim also invalidates the active handle.
		QVERIFY(!clip.isPlaying());
		for (auto handle : engine->playHandles()) { QVERIFY(!handle->isFromTrack(&track)); }
	}

	void trimmingPreservesRecordingHandle()
	{
		SampleTrack track(Engine::getSong());
		SampleClip recording(&track);
		SampleClip audio(&track);
		loadSample(audio);
		recording.setRecord(true);
		auto song = Engine::getSong();
		auto engine = Engine::audioEngine();
		song->record();
		// Register an active recording with its bus, then exercise the real engine.
		auto handle = new SampleRecordHandle(&recording);
		handle->setAudioBusHandle(track.audioBusHandle());
		QVERIFY(engine->addPlayHandle(handle));
		recording.setIsPlaying(true);
		engine->renderNextPeriod();
		QVERIFY(audio.trim(150, 150));
		QVERIFY(recording.isPlaying());
		QVERIFY(recording.isRecord());
		QVERIFY(std::find(engine->playHandles().begin(), engine->playHandles().end(), handle)
			!= engine->playHandles().end());
		QVERIFY(recording.trim(0, 500)); // Resizing the recording itself also preserves it.
		QVERIFY(recording.isPlaying());
		QVERIFY(std::find(engine->playHandles().begin(), engine->playHandles().end(), handle)
			!= engine->playHandles().end());
		song->stop();
		engine->renderNextPeriod();
	}
};

QTEST_GUILESS_MAIN(SampleClipTrimTest)
#include "SampleClipTrimTest.moc"
