/*  Device Manager
    Copyright (C) 2003 Jasem Mutlaq (mutlaqja@ikarustech.com)

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    JM Changelog
    2004-16-1:	Start

 */

#include "devicemanager.h"

#include "Options.h"
#include "indimenu.h"
#include "indiproperty.h"
#include "indigroup.h"
#include "indidevice.h"
#include "indi/indicom.h"
#include "kstars.h"
#include "kstarsdatetime.h"
#include <config-kstars.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>

#include <QSocketNotifier>
#include <QTextEdit>

#include <klocale.h>
#include <kdebug.h>
#include <kmessagebox.h>
#include <kstatusbar.h>


 /*******************************************************************
** The device manager contain devices running from one indiserver
** This allow KStars to control multiple devices distributed acorss
** multiple servers seemingly in a way that is completely transparent
** to devices and drivers alike.
** The device Manager can be thought of as the 'networking' parent
** of devices, while indimenu is 'GUI' parent of devices
*******************************************************************/

DeviceManager::DeviceManager(INDIMenu *INDIparent, int inID)
{

 parent = INDIparent;
 mgrID  = inID;

// indi_dev.setAutoDelete(true);

 serverFD  = -1;
 serverFP  = NULL;
 XMLParser = NULL;
 sNotifier = NULL;

}

DeviceManager::~DeviceManager()
{

   if (serverFP)
    	fclose(serverFP);

  if (serverFD >= 0)
    close(serverFD);

  if (XMLParser)
     delLilXML(XMLParser);

  XMLParser = NULL;

	while ( ! indi_dev.isEmpty() ) delete indi_dev.takeFirst();
}

bool DeviceManager::indiConnect( const QString &inHost, const QString &inPort )
{
        host = inHost;
	port = inPort;
	QString errMsg;
	struct sockaddr_in pin;
	struct hostent *serverHostName = gethostbyname(host.toAscii());
	errMsg = QString("Connection to INDI host at %1 on port %2 failed.").arg(host).arg(port);

	memset(&pin, 0, sizeof(pin));
	pin.sin_family 		= AF_INET;
	pin.sin_addr.s_addr 	= ((struct in_addr *) (serverHostName->h_addr))->s_addr;
	pin.sin_port 		= htons(port.toInt());

	if ( (serverFD = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
	 KMessageBox::error(0, i18n("Cannot create socket"));
	 return false;
	}

	if ( ::connect(serverFD, (struct sockaddr*) &pin, sizeof(pin)) == -1)
	{
	  //KMessageBox::error(0, errMsg);
	  serverFD = -1;
	  return false;
	}

	// callback notified
	sNotifier = new QSocketNotifier( serverFD, QSocketNotifier::Read, this);
        QObject::connect( sNotifier, SIGNAL(activated(int)), this, SLOT(dataReceived()));

	if (XMLParser)
	   delLilXML(XMLParser);
	XMLParser = newLilXML();

	// ready for fprintf
	serverFP = fdopen(serverFD, "w");

	if (serverFP == NULL)
	{
	 KMessageBox::error(0, i18n("Cannot read server file descriptor"));
	 serverFD = -1;
	 return false;
	}

	setbuf (serverFP, NULL);

	fprintf(serverFP, "<enableBLOB>Also</enableBLOB>\n");
	fprintf(serverFP, "<getProperties version='%g'/>\n", INDIVERSION);

	// We made it!
	return true;
}


void DeviceManager::dataReceived()
{
	char ibuf[32];	/* not so much user input lags */
	char errmsg[ERRMSG_SIZE];
	QString cmd_err;
	int i, nr;

	/* read INDI command */
	nr = read (serverFD, ibuf, sizeof(ibuf)-1);
	if (nr <= 0)
	{
	    if (nr < 0)
		cmd_err = QString("INDI: input error.");
	    else
		cmd_err = QString("INDI: agent closed connection.");


#ifdef HAVE_TERMIOS_H
            tcflush(serverFD, TCIFLUSH);
#endif
	    sNotifier->disconnect();
	    close(serverFD);
	    parent->removeDeviceMgr(mgrID);
	    KMessageBox::error(0, cmd_err);

            return;
	}

        ibuf[ sizeof( ibuf )-1 ] = '\0';

	/* process each char */
	for (i = 0; i < nr; i++)
	{
  	  if (!XMLParser)
	     	return;

	    XMLEle *root = readXMLEle (XMLParser, (int)ibuf[i], errmsg);
	    if (root)
	    {
                //prXMLEle (stdout, root, 0);
		if (dispatchCommand(root, cmd_err) < 0)
		{
		    kDebug() << cmd_err << endl;
		    prXMLEle (stdout, root, 0);
		}

		delXMLEle (root);
	    }
	    else if (*errmsg)
	    {
		kDebug() << errmsg << endl;
	    }
	}
}

int DeviceManager::dispatchCommand(XMLEle *root, QString & errmsg)
{

  if  (!strcmp (tagXMLEle(root), "message"))
    	 return messageCmd(root, errmsg);
  else if  (!strcmp (tagXMLEle(root), "delProperty"))
        return delPropertyCmd(root, errmsg);

  /* Get the device, if not available, create it */
  INDI_D *dp = findDev (root, 1, errmsg);
      if (dp == NULL)
	    return -1;

   if (!strcmp (tagXMLEle(root), "defTextVector"))
      return dp->buildTextGUI(root, errmsg);
   else if (!strcmp (tagXMLEle(root), "defNumberVector"))
      return dp->buildNumberGUI(root, errmsg);
   else if (!strcmp (tagXMLEle(root), "defSwitchVector"))
       return dp->buildSwitchesGUI(root, errmsg);
   else if (!strcmp (tagXMLEle(root), "defLightVector"))
        return dp->buildLightsGUI(root, errmsg);
   else if (!strcmp (tagXMLEle(root), "defBLOBVector"))
     return dp->buildBLOBGUI(root, errmsg);
   else if (!strcmp (tagXMLEle(root), "setTextVector") ||
   	    !strcmp (tagXMLEle(root), "setNumberVector") ||
	    !strcmp (tagXMLEle(root), "setSwitchVector") ||
	    !strcmp (tagXMLEle(root), "setLightVector") ||
	    !strcmp (tagXMLEle(root), "setBLOBVector"))
	return dp->setAnyCmd(root, errmsg);

   return (-1);
}

/* delete the property in the given device, including widgets and data structs.
 * when last property is deleted, delete the device too.
 * if no property name attribute at all, delete the whole device regardless.
 * return 0 if ok, else -1 with reason in errmsg[].
 */
int DeviceManager::delPropertyCmd (XMLEle *root, QString & errmsg)
{

	XMLAtt *ap;
	INDI_D *dp;
	INDI_P *pp;

	/* dig out device and optional property name */
	dp = findDev (root, 0, errmsg);
	if (!dp)
	    return (-1);

	checkMsg(root, dp);

	ap = findXMLAtt (root, "name");

	/* Delete property if it exists, otherwise, delete the whole device */
	if (ap)
	{
	  pp = dp->findProp(QString(valuXMLAtt(ap)));

	 if(pp)
	 return dp->removeProperty(pp);
	    else
	      return (-1);
	}
	// delete the whole device
	else
            return removeDevice(dp->name, errmsg);

}

int DeviceManager::removeDevice( const QString &devName, QString & errmsg )
{

    // remove all devices if devName == NULL
    if (devName == NULL)
    {
        while ( ! indi_dev.isEmpty() ) delete indi_dev.takeFirst();
	return (0);
    }

    for (int i=0; i < indi_dev.size(); i++)
    {
         if (indi_dev[i]->name ==  devName)
	 {
	    delete indi_dev.takeAt(i);
            return (0);
	 }
    }

   errmsg = QString("Device %1 not found").arg(devName);
   return -1;
}

INDI_D * DeviceManager::findDev( const QString &devName, QString & errmsg )
{
	/* search for existing */
	for (int i = 0; i < indi_dev.size(); i++)
	{
	    if (indi_dev[i]->name == devName)
		return indi_dev[i];
	}

	errmsg = QString("INDI: no such device %1").arg(devName);

	return NULL;
}

/* add new device to mainrc_w using info in dep.
- * if trouble return NULL with reason in errmsg[]
- */
INDI_D * DeviceManager::addDevice (XMLEle *dep, QString & errmsg)
{
	INDI_D *dp;
	XMLAtt *ap;

	/* allocate new INDI_D on indi_dev */
	ap = findAtt (dep, "device", errmsg);
	if (!ap)
	    return NULL;

	if (parent->currentLabel.isEmpty())
	 parent->setCustomLabel(valuXMLAtt(ap));

	dp = new INDI_D(parent, this, QString(valuXMLAtt(ap)), parent->currentLabel);

	indi_dev.append(dp);

	emit newDevice();

	// Reset label
	parent->currentLabel = QString();

       /* ok */
	return dp;
}

INDI_D * DeviceManager::findDev (XMLEle *root, int create, QString & errmsg)
{
	XMLAtt *ap;
	char *dn;

	/* get device name */
	ap = findAtt (root, "device", errmsg);
	if (!ap)
	    return (NULL);
	dn = valuXMLAtt(ap);

	/* search for existing */
	for (int i = 0; i < indi_dev.size(); i++)
	{
	    if (indi_dev[i]->name == QString(dn))
		return indi_dev[i];
	}

	/* not found, create if ok */
	if (create)
		return (addDevice (root, errmsg));

	errmsg = QString("INDI: <%1> no such device %2").arg(tagXMLEle(root)).arg(dn);
	return NULL;
}

/* a general message command received from the device.
 * return 0 if ok, else -1 with reason in errmsg[].
 */
int DeviceManager::messageCmd (XMLEle *root, QString & errmsg)
{
	checkMsg (root, findDev (root, 0, errmsg));
	return (0);
}

/* display message attribute.
 * N.B. don't put carriage control in msg, we take care of that.
 */
void DeviceManager::checkMsg (XMLEle *root, INDI_D *dp)
{
	XMLAtt *ap;
	ap = findXMLAtt(root, "message");

	if (ap)
	  doMsg(root, dp);
}

/* display valu of message and timestamp in dp's scrolled area, if any, else general.
 * prefix our time stamp if not included.
 * N.B. don't put carriage control in msg, we take care of that.
 */
void DeviceManager::doMsg (XMLEle *msg, INDI_D *dp)
{
	QTextEdit *txt_w;
	XMLAtt *message;
	XMLAtt *timestamp;

        if (dp == NULL)
	{
	  kDebug() << "Warning: dp is null." << endl;
	  return;
	}

	txt_w = dp->msgST_w;

	/* prefix our timestamp if not with msg */
	timestamp = findXMLAtt (msg, "timestamp");

	if (timestamp)
	   txt_w->insertPlainText(QString(valuXMLAtt(timestamp)) + QString(" "));
	else
	   txt_w->insertPlainText( KStarsDateTime::currentDateTime().toString("yyyy/mm/dd - h:m:s ap "));

	/* finally! the msg */
        message = findXMLAtt(msg, "message");

        if (!message) return;

       txt_w->insertPlainText( QString(valuXMLAtt(message)) + QString("\n"));

       if ( Options::indiMessages() )
	    	parent->ksw->statusBar()->changeItem( QString(valuXMLAtt(message)), 0);

}

void DeviceManager::sendNewText (INDI_P *pp)
{
        INDI_E *lp;

	fprintf(serverFP, "<newTextVector\n");
	fprintf(serverFP, "  device='%s'\n",qPrintable( pp->pg->dp->name));
	fprintf(serverFP, "  name='%s'\n>",qPrintable( pp->name));

	//for (lp = pp->el.first(); lp != NULL; lp = pp->el.next())
	foreach(lp, pp->el)
	{
	    fprintf(serverFP, "  <oneText\n");
	    fprintf(serverFP, "    name='%s'>\n",qPrintable( lp->name));
	    fprintf(serverFP, "      %s\n",qPrintable( lp->text));
	    fprintf(serverFP, "  </oneText>\n");
	}
	fprintf(serverFP, "</newTextVector>\n");
}

void DeviceManager::sendNewNumber (INDI_P *pp)
{
        INDI_E *lp;

        fprintf(serverFP, "<newNumberVector\n");
	fprintf(serverFP, "  device='%s'\n",qPrintable( pp->pg->dp->name));
	fprintf(serverFP, "  name='%s'\n>",qPrintable( pp->name));

	//for (lp = pp->el.first(); lp != NULL; lp = pp->el.next())
        foreach(lp, pp->el)
	{
	    fprintf(serverFP, "  <oneNumber\n");
	    fprintf(serverFP, "    name='%s'>\n",qPrintable( lp->name));
	    fprintf(serverFP, "      %g\n", lp->targetValue);
	    fprintf(serverFP, "  </oneNumber>\n");
	}
	fprintf(serverFP, "</newNumberVector>\n");

}

void DeviceManager::sendNewSwitch (INDI_P *pp, int index)
{
        INDI_E *lp;
	//int i=0;

	fprintf (serverFP,"<newSwitchVector\n");
	fprintf (serverFP,"  device='%s'\n",qPrintable( pp->pg->dp->name));
	fprintf (serverFP,"  name='%s'>\n",qPrintable( pp->name));

        lp = pp->el[index];
	//for (lp = pp->el.first(); lp != NULL; lp = pp->el.next(), i++)
          //foreach(lp, pp->el)
          //{
	  	//if (i == index)
          	//{
	    		fprintf (serverFP,"  <oneSwitch\n");
	    		fprintf (serverFP,"    name='%s'>\n",qPrintable( lp->name));
	    		fprintf (serverFP,"      %s\n", lp->state == PS_ON ? "On" : "Off");
	    		fprintf (serverFP,"  </oneSwitch>\n");
	    	//	break;
	  	//}
	  //}

	fprintf (serverFP, "</newSwitchVector>\n");

}

void DeviceManager::startBlob( const QString &devName, const QString &propName, const QString &timestamp)
{

   fprintf (serverFP, "<newBLOBVector\n");
   fprintf (serverFP, "  device='%s'\n",qPrintable( devName));
   fprintf (serverFP, "  name='%s'\n",qPrintable( propName));
   fprintf (serverFP, "  timestamp='%s'>\n",qPrintable( timestamp));

}

void DeviceManager::sendOneBlob( const QString &blobName, unsigned int blobSize, const QString &blobFormat, unsigned char * blobBuffer)
{

 fprintf (serverFP, "  <oneBLOB\n");
 fprintf (serverFP, "    name='%s'\n",qPrintable( blobName));
 fprintf (serverFP, "    size='%d'\n", blobSize);
 fprintf (serverFP, "    format='%s'>\n",qPrintable( blobFormat));

  for (unsigned i = 0; i < blobSize; i += 72)
	fprintf (serverFP, "    %.72s\n", blobBuffer+i);

  fprintf (serverFP, "  </oneBLOB>\n");

}

void DeviceManager::finishBlob()
{
       fprintf (serverFP, "</newBLOBVector>\n");
}


#include "devicemanager.moc"
