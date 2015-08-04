/***************************************************************************
                          skyguidemgr.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : 2015/05/06
    copyright            : (C) 2015 by Marcos Cardinot
    email                : mcardinot@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileDialog>
#include <QStandardPaths>
#include <kzip.h>

#include "skyguidemgr.h"

SkyGuideMgr::SkyGuideMgr()
    : m_view(new SkyGuideView())
{
    m_guidesDir = QDir(QStandardPaths::locate(QStandardPaths::DataLocation,
            "tools/skyguide/resources/guides", QStandardPaths::LocateDirectory));

    loadAllSkyGuideObjects();
    connect((QObject*)m_view->rootObject(), SIGNAL(addSkyGuide()), this, SLOT(slotAddSkyGuide()));
}

SkyGuideMgr::~SkyGuideMgr()
{
}

void SkyGuideMgr::loadAllSkyGuideObjects() {
    QDir root = m_guidesDir;
    QDir::Filters filters = QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks;
    root.setFilter(filters | QDir::Dirs);
    QFileInfoList guidesRoot = root.entryInfoList();
    foreach (QFileInfo r, guidesRoot) {
        QDir guideDir(r.filePath());
        guideDir.setFilter(filters | QDir::Files);
        QFileInfoList guideFiles = guideDir.entryInfoList();
        foreach (QFileInfo g, guideFiles) {
            if (g.fileName() == JSON_NAME) {
                SkyGuideObject* s = buildSkyGuideObject(g.absoluteFilePath());
                loadSkyGuideObject(s);
            }
        }
    }
    m_view->setModel(m_skyGuideObjects);
}

bool SkyGuideMgr::loadSkyGuideObject(SkyGuideObject* skyGuideObj) {
    if (!skyGuideObj || !skyGuideObj->isValid()) {
        return false;
    }

    // title is unique?
    foreach (QObject* sg, m_skyGuideObjects) {
        if (((SkyGuideObject*)sg)->title() == skyGuideObj->title()) {
            qWarning()  << "SkyGuideMgr: The title '"
                        << skyGuideObj->title()
                        << "' is being used already.";
            return false;
        }
    }

    m_skyGuideObjects.append(skyGuideObj);
    return true;
}

SkyGuideObject* SkyGuideMgr::buildSkyGuideObject(const QString& jsonPath) {
    QFileInfo info(jsonPath);
    if (info.fileName() != JSON_NAME || !info.exists()) {
        qWarning() << "SkyGuideMgr: The JSON file is invalid or does not exist!"
                   << jsonPath;
        return NULL;
    }

    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        qWarning() << "SkyGuideMgr: Couldn't open the JSON file!" << jsonPath;
        return NULL;
    }

    QJsonObject json(QJsonDocument::fromJson(jsonFile.readAll()).object());
    jsonFile.close();

    SkyGuideObject* s = new SkyGuideObject(info.absolutePath(), json.toVariantMap());
    if (!s->isValid()) {
        qWarning()  << "SkyGuideMgr: SkyGuide is invalid!" << jsonPath;
        return NULL;
    }
    return s;
}

void SkyGuideMgr::slotAddSkyGuide() {
    // check if the installation dir is writable
    if (!QFileInfo(m_guidesDir.absolutePath()).isWritable()){
        qWarning() << "SkyGuideMgr: The installation directory must be writable!"
                   << m_guidesDir.absolutePath();
        return;
    }

    // open QFileDialog - select the SkyGuide
    QString desktop = QStandardPaths::standardLocations(QStandardPaths::DesktopLocation).first();
    QString path = QFileDialog::getOpenFileName(NULL, "Add SkyGuide", desktop, "Zip File (*.zip)");

    // try to open the SkyGuide archive
    KZip archive(path);
    if (!archive.open(QIODevice::ReadOnly)) {
        qWarning() << "SkyGuideMgr: Unable to read the file!"
                   << "Is it a zip archive?" << path;
        return;
    }

    // check if this SkyGuide has a 'guide.json' file in the root
    const QString jsonFilename = "guide.json";
    const KArchiveDirectory *root = archive.directory();
    const KArchiveEntry *e = root->entry(jsonFilename);
    if (!e) {
        qWarning() << "SkyGuideMgr: 'guide.json' not found!"
                   << "A SkyGuide must have a 'guide.json' in the root!";
        return;
    }

    // create a clean /temp folder
    QDir tmpDir(m_guidesDir.absolutePath() + "/temp");
    tmpDir.removeRecursively();
    m_guidesDir.mkdir("temp");

    //  copy files from archive to the /temp folder
    root->copyTo(tmpDir.absolutePath(), true);
    archive.close();

    // build the SkyGuideObject
    SkyGuideObject* sObj = buildSkyGuideObject(tmpDir.absoluteFilePath(JSON_NAME));

    // try to load it!
    if (!loadSkyGuideObject(sObj)) {
        tmpDir.removeRecursively();
        return;
    }

    // rename the temp folder
    int i = 0;
    SkyGuideObject* guide = ((SkyGuideObject*) m_skyGuideObjects.last());
    QString title = guide->title();
    while (!m_guidesDir.rename("temp", title)) {
        title += QString::number(i);  // doesn't matter the number
        i++;
    }

    // fix path
    guide->setPath(m_guidesDir.absoluteFilePath(title));

    // refresh view
    m_view->setModel(m_skyGuideObjects);
}