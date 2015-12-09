/*
 * Copyright (c) 2013-2015 Meltytech, LLC
 * Author: Dan Dennedy <dan@dennedy.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "audiolevelstask.h"
#include "database.h"
#include "mltcontroller.h"
#include "shotcut_mlt_properties.h"
#include <QString>
#include <QVariantList>
#include <QImage>
#include <QCryptographicHash>
#include <QRgb>
#include <QThreadPool>
#include <QMutex>

static QList<AudioLevelsTask*> tasksList;
static QMutex tasksListMutex;

static void deleteQVariantList(QVariantList* list)
{
    delete list;
}

AudioLevelsTask::AudioLevelsTask(Mlt::Producer& producer, MultitrackModel* model, const QModelIndex& index)
    : QRunnable()
    , m_model(model)
    , m_tempProducer(0)
    , m_isCanceled(false)
{
    m_producers << ProducerAndIndex(new Mlt::Producer(producer), index);
}

AudioLevelsTask::~AudioLevelsTask()
{
    delete m_tempProducer;
    foreach (ProducerAndIndex p, m_producers)
        delete p.first;
}

void AudioLevelsTask::start(Mlt::Producer& producer, MultitrackModel* model, const QModelIndex& index)
{
    if (producer.is_valid() && index.isValid()) {
        AudioLevelsTask* task = new AudioLevelsTask(producer, model, index);
        tasksListMutex.lock();
        // See if there is already a task for this MLT service and resource.
        foreach (AudioLevelsTask* t, tasksList) {
            if (*t == *task) {
                // If so, then just add ourselves to be notified upon completion.
                delete task;
                task = 0;
                t->m_producers << ProducerAndIndex(new Mlt::Producer(producer), index);
                break;
            }
        }
        if (task) {
            // Otherwise, start a new audio levels generation thread.
            tasksList << task;
            QThreadPool::globalInstance()->start(task);
        }
        tasksListMutex.unlock();
    }
}

void AudioLevelsTask::closeAll()
{
    // Tell all of the audio levels tasks to stop.
    tasksListMutex.lock();
    while (!tasksList.isEmpty()) {
        AudioLevelsTask* task = tasksList.first();
        task->m_isCanceled = true;
        tasksList.removeFirst();
    }
    tasksListMutex.unlock();
}

bool AudioLevelsTask::operator==(AudioLevelsTask &b)
{
    if (!m_producers.isEmpty() && !b.m_producers.isEmpty()) {
        Mlt::Producer* a_producer = m_producers.first().first;
        Mlt::Producer* b_producer = b.m_producers.first().first;
        return !qstrcmp(a_producer->get("resource"), b_producer->get("resource"));
    }
    return false;
}

Mlt::Producer* AudioLevelsTask::tempProducer()
{
    if (!m_tempProducer) {
        QString service = m_producers.first().first->get("mlt_service");
        if (service == "avformat-novalidate")
            service = "avformat";
        else if (service.startsWith("xml"))
            service = "xml-nogl";
        m_tempProducer = new Mlt::Producer(MLT.profile(), service.toUtf8().constData(),
            m_producers.first().first->get("resource"));
        if (m_tempProducer->is_valid()) {
            Mlt::Filter channels(MLT.profile(), "audiochannels");
            Mlt::Filter converter(MLT.profile(), "audioconvert");
            Mlt::Filter levels(MLT.profile(), "audiolevel");
            m_tempProducer->attach(channels);
            m_tempProducer->attach(converter);
            m_tempProducer->attach(levels);
        }
    }
    return m_tempProducer;
}

QString AudioLevelsTask::cacheKey()
{
    QString key = QString("%1 audiolevels").arg(m_producers.first().first->get("resource"));
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(key.toUtf8());
    return hash.result().toHex();
}

void AudioLevelsTask::run()
{
    int n = tempProducer()->get_playtime();
    // 2 channels interleaved of uchar values
    QVariantList levels;
    QImage image = DB.getThumbnail(cacheKey());
    if (image.isNull()) {
        const char* key[2] = { "meta.media.audio_level.0", "meta.media.audio_level.1"};
        // TODO: use project channel count
        int channels = 2;

        // for each frame
        for (int i = 0; i < n && !m_isCanceled; i++) {
            Mlt::Frame* frame = m_tempProducer->get_frame();
            if (frame && frame->is_valid() && !frame->get_int("test_audio")) {
                mlt_audio_format format = mlt_audio_s16;
                int frequency = 48000;
                int samples = mlt_sample_calculator(m_producers.first().first->get_fps(), frequency, i);
                frame->get_audio(format, frequency, channels, samples);
                // for each channel
                for (int channel = 0; channel < channels; channel++)
                    // Convert real to uint for caching as image.
                    // Scale by 0.9 because values may exceed 1.0 to indicate clipping.
                    levels << 256 * qMin(frame->get_double(key[channel]) * 0.9, 1.0);
            } else if (!levels.isEmpty()) {
                for (int channel = 0; channel < channels; channel++)
                    levels << levels.last();
            }
            delete frame;
        }
        if (levels.size() > 0 && !m_isCanceled) {
            // Put into an image for caching.
            int count = levels.size();
            QImage image((count + 3) / 4, channels, QImage::Format_ARGB32);
            n = image.width() * image.height();
            for (int i = 0; i < n; i ++) {
                QRgb p;
                if ((4*i + 3) < count) {
                    p = qRgba(levels.at(4*i).toInt(), levels.at(4*i+1).toInt(), levels.at(4*i+2).toInt(), levels.at(4*i+3).toInt());
                } else {
                    int last = levels.last().toInt();
                    int r = (4*i+0) < count? levels.at(4*i+0).toInt() : last;
                    int g = (4*i+1) < count? levels.at(4*i+1).toInt() : last;
                    int b = (4*i+2) < count? levels.at(4*i+2).toInt() : last;
                    int a = last;
                    p = qRgba(r, g, b, a);
                }
                image.setPixel(i / 2, i % channels, p);
            }
            DB.putThumbnail(cacheKey(), image);
        }
    } else if (!m_isCanceled) {
        // convert cached image
        int channels = 2;
        int n = image.width() * image.height();
        for (int i = 0; i < n; i++) {
            QRgb p = image.pixel(i / 2, i % channels);
            levels << qRed(p);
            levels << qGreen(p);
            levels << qBlue(p);
            levels << qAlpha(p);
        }
    }

    // Remove ourself from the global list of audio tasks.
    tasksListMutex.lock();
    for (int i = 0; i < tasksList.size(); ++i) {
        if (*tasksList[i] == *this) {
            tasksList.removeAt(i);
            break;
        }
    }
    tasksListMutex.unlock();

    if (levels.size() > 0 && !m_isCanceled) {
        foreach (ProducerAndIndex p, m_producers) {
            QVariantList* levelsCopy = new QVariantList(levels);
            p.first->set(kAudioLevelsProperty, levelsCopy, 0, (mlt_destructor) deleteQVariantList);
            m_model->audioLevelsReady(p.second);
        }
    }
}