#include <QtTest>
#include <QScopeGuard>

#include "AudioDevice.h"
#include "AudioDummy.h"
#include "ComboBoxModel.h"
#include "ConfigManager.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "MidiDummy.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleClipView.h"
#include "SampleTrack.h"
#include "Song.h"
#include "SongEditor.h"

using namespace lmms;
using namespace lmms::gui;

class SampleClipViewTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_directory;
	GuiApplication* m_gui = nullptr;

	static SampleClipView* viewFor(SampleClip* clip)
	{
		QCoreApplication::processEvents();
		for (auto view : getGUI()->songEditor()->m_editor->findChildren<SampleClipView*>())
		{
			if (view->getClip() == clip) { return view; }
		}
		return nullptr;
	}

	static SampleClip* createClip()
	{
		auto track = new SampleTrack(Engine::getSong());
		auto clip = new SampleClip(track);
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(100250), 48000));
		clip->movePosition(100);
		QCoreApplication::processEvents();
		return clip;
	}

	static void moveMouse(SampleClipView* view, QPoint global, Qt::KeyboardModifiers modifiers)
	{
		QMouseEvent move(QEvent::MouseMove, view->mapFromGlobal(global), global,
			Qt::NoButton, Qt::LeftButton, modifiers);
		QApplication::sendEvent(view, &move);
	}

	static void drag(SampleClipView* view, QPoint from, int delta, Qt::KeyboardModifiers modifiers)
	{
		const auto target = view->mapToGlobal(from + QPoint(delta, 0));
		QTest::mousePress(view, Qt::LeftButton, modifiers, from);
		moveMouse(view, target, modifiers);
		QTest::mouseRelease(view, Qt::LeftButton, modifiers, view->mapFromGlobal(target));
	}

private slots:
	void initTestCase()
	{
		QVERIFY(m_directory.isValid());
		auto config = ConfigManager::inst();
		config->setWorkingDir(m_directory.path());
		config->setValue("app", "configured", "1");
		config->setValue("ui", "enableautosave", "0");
		config->setValue("audioengine", "audiodev", AudioDummy::name());
		config->setValue("audioengine", "mididev", MidiDummy::name());
		m_gui = new GuiApplication;
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::getSong()->setTempo(120);
	}

	void cleanupTestCase()
	{
		m_gui->mainWindow()->deleteLater(); // Owns engine shutdown, as in the application.
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		delete m_gui;
	}

	void init()
	{
		Engine::getSong()->clearProject();
		Engine::getSong()->setTempo(120);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		auto editor = m_gui->songEditor()->m_editor;
		editor->setPixelsPerBar(TimePos::ticksPerBar()); // One pixel per tick.
		editor->setEditModeDraw();
	}

	void mouseTrimIsOneUndoAction()
	{
		auto clip = createClip();
		auto view = viewFor(clip);
		QVERIFY(view);
		auto journal = Engine::projectJournal();
		journal->clearJournal();
		const QPoint grip(1, view->height() / 2);
		const auto target = view->mapToGlobal(grip + QPoint(50, 0));
		QTest::mousePress(view, Qt::LeftButton, Qt::AltModifier, grip);
		QVERIFY(!clip->isJournalling());
		moveMouse(view, target - QPoint(25, 0), Qt::AltModifier);
		moveMouse(view, target, Qt::AltModifier);
		QTest::mouseRelease(view, Qt::LeftButton, Qt::AltModifier, view->mapFromGlobal(target));
		QCOMPARE(clip->startPosition().getTicks(), 150);
		QCOMPARE(clip->length().getTicks(), 150);
		QCOMPARE(clip->startTimeOffset().getTicks(), -50);
		QVERIFY(clip->isJournalling());
		QVERIFY(clip->isJournallingStateStackEmpty());
		journal->undo();
		QCOMPARE(clip->startPosition().getTicks(), 100);
		QCOMPARE(clip->length().getTicks(), 200);
		QVERIFY(!journal->canUndo());
		journal->redo();
		QCOMPARE(clip->startPosition().getTicks(), 150);
		QCOMPARE(clip->length().getTicks(), 150);

		// Restoring a deliberately disabled journal must not enable it.
		clip->setJournalling(false);
		drag(view, QPoint(view->width() - 2, grip.y()), -50, Qt::AltModifier);
		QVERIFY(!clip->isJournalling());
		QVERIFY(clip->isJournallingStateStackEmpty());
		clip->setJournalling(true);
	}

	void knifeSplitTrimExtendAndReopen()
	{
		auto clip = createClip();
		auto view = viewFor(clip);
		QVERIFY(view);
		auto editor = m_gui->songEditor()->m_editor;
		editor->setEditModeKnife();
		QTest::mouseClick(view, Qt::LeftButton, Qt::AltModifier, QPoint(80, view->height() / 2));
		auto track = static_cast<SampleTrack*>(clip->getTrack());
		QCOMPARE(track->numOfClips(), 2);
		auto right = static_cast<SampleClip*>(track->getClip(1));
		QCOMPARE(right->startPosition().getTicks(), 180);
		QCOMPARE(right->startTimeOffset().getTicks(), -80);
		editor->setEditModeDraw();
		auto rightView = viewFor(right);
		QVERIFY(rightView);
		drag(rightView, QPoint(1, rightView->height() / 2), 30, Qt::AltModifier);
		QCOMPARE(right->startPosition().getTicks(), 210);
		drag(rightView, QPoint(rightView->width() - 2, rightView->height() / 2), -40, Qt::AltModifier);
		QVERIFY(right->endPosition() < 300);
		drag(rightView, QPoint(rightView->width() - 2, rightView->height() / 2), 500, Qt::AltModifier);
		QCOMPARE(right->endPosition().getTicks(), 300);
		drag(rightView, QPoint(1, rightView->height() / 2), -500, Qt::AltModifier);
		QCOMPARE(right->startPosition().getTicks(), 100);
		QCOMPARE(right->startTimeOffset().getTicks(), 0);
		drag(rightView, QPoint(1, rightView->height() / 2), 25, Qt::AltModifier);
		drag(rightView, QPoint(rightView->width() - 2, rightView->height() / 2), -40, Qt::AltModifier);
		const auto savedPosition = right->startPosition();
		const auto savedLength = right->length();
		const auto savedOffset = right->startTimeOffset();
		QVERIFY(savedOffset < 0);
		const auto file = m_directory.filePath("split-trim.mmp");
		QVERIFY(Engine::getSong()->guiSaveProjectAs(file));
		Engine::getSong()->loadProject(file);
		auto loadedTrack = Engine::getSong()->tracks().front();
		QCOMPARE(loadedTrack->numOfClips(), 2);
		auto loaded = static_cast<SampleClip*>(loadedTrack->getClip(1));
		QCOMPARE(loaded->startPosition(), savedPosition);
		QCOMPARE(loaded->length(), savedLength);
		QCOMPARE(loaded->startTimeOffset(), savedOffset);
		QVERIFY(!loaded->getAutoResize());
	}

	void snappedEdgesAndSelectedClip()
	{
		auto clip = createClip();
		auto view = viewFor(clip);
		QVERIFY(view);
		auto editor = m_gui->songEditor()->m_editor;
		QVERIFY(editor->getSnapSize() > 0);
		drag(view, QPoint(view->width() - 2, view->height() / 2), -100, Qt::NoModifier);
		QVERIFY(clip->length() < 200);
		drag(view, QPoint(view->width() - 2, view->height() / 2), 500, Qt::ShiftModifier);
		QCOMPARE(clip->endPosition().getTicks(), 300);
		drag(view, QPoint(1, view->height() / 2), 70, Qt::NoModifier);
		QVERIFY(clip->startPosition() > 100);
		drag(view, QPoint(1, view->height() / 2), -500, Qt::ShiftModifier);
		QCOMPARE(clip->startPosition().getTicks(), 100);
		view->setSelected(true);
		const auto length = clip->length();
		const auto offset = clip->startTimeOffset();
		drag(view, QPoint(1, view->height() / 2), 50, Qt::AltModifier);
		QCOMPARE(clip->length(), length);
		QCOMPARE(clip->startTimeOffset(), offset);
	}

	void trimDuringPlayback()
	{
		auto clip = createClip();
		clip->movePosition(0);
		auto view = viewFor(clip);
		QVERIFY(view);
		auto song = Engine::getSong();
		auto engine = Engine::audioEngine();
		const auto stopPlayback = qScopeGuard([&]
		{
			engine->audioDev()->stopProcessing();
			song->stop();
		});
		song->playSong();
		engine->audioDev()->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(([&]
		{
			const auto guard = engine->requestChangesGuard();
			return clip->isPlaying();
		})(), 2000);
		drag(view, QPoint(1, view->height() / 2), 40, Qt::AltModifier);
		QCOMPARE(clip->startPosition().getTicks(), 40);
		QCOMPARE(clip->startTimeOffset().getTicks(), -40);
		drag(view, QPoint(view->width() - 2, view->height() / 2), -50, Qt::AltModifier);
		QVERIFY(clip->endPosition() < 200);
		QVERIFY(song->isPlaying());
	}
};

QTEST_MAIN(SampleClipViewTest)
#include "SampleClipViewTest.moc"
