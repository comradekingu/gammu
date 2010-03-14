
#include "../../cfg/config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#ifdef WIN32
#  include <io.h>
#endif
#if defined HAVE_DIRENT_H && defined HAVE_SCANDIR && defined HAVE_ALPHASORT
#  include <dirent.h>
#endif

#include "../../common/service/gsmback.h"
#include "../../common/misc/coding.h"
#include "smsdcore.h"

/* Save SMS from phone (called Inbox sms - it's in phone Inbox) somewhere */
static GSM_Error SMSDFiles_SaveInboxSMS(GSM_MultiSMSMessage sms, GSM_SMSDConfig *Config)
{
	GSM_Error	error = GE_NONE;
	int 		i,j;
	unsigned char 	FileName[32], FullName[400], ext[4], buffer[64],buffer2[400];
 	bool		done;
	FILE 		*file;
	GSM_SMS_Backup 	backup;

	if (mystrncasecmp(Config->inboxformat, "detail", 0)) {
		for (i=0;i<sms.Number;i++) backup.SMS[i] = &sms.SMS[i];
		backup.SMS[sms.Number] = NULL;
	}

	j = 0;
	done = false;
	for (i=0;i<sms.Number && !done;i++) {
		strcpy(ext, "txt");
		if ((sms.SMS[i].Coding == GSM_Coding_8bit) && (!mystrncasecmp(Config->inboxformat, "detail", 0)))
		{
			strcpy(ext, "bin");
		}
		DecodeUnicode(sms.SMS[i].Number,buffer2);
		/* we loop on yy for the first SMS assuming that if xxxx_yy_00.ext is absent,
		   any xxxx_yy_01,02, must be garbage, that can be overwritten */
		file = NULL;
		do {
			sprintf(FileName,
				"IN%02d%02d%02d_%02d%02d%02d_%02i_%s_%02i.%s",
				sms.SMS[i].DateTime.Year, sms.SMS[i].DateTime.Month,  sms.SMS[i].DateTime.Day,
				sms.SMS[i].DateTime.Hour, sms.SMS[i].DateTime.Minute, sms.SMS[i].DateTime.Second,
				j, buffer2, i, ext);
			strcpy(FullName, Config->inboxpath);
			strcat(FullName, FileName);
			if (file) fclose(file);
			file = fopen(FullName, "r");
		} while ((i == 0) && (file && (++j < 100)));
		if (file) {
			fclose(file);
			if (i == 0) {
				WriteSMSDLog("Cannot save %s. No available file names", FileName);
				return GE_CANTOPENFILE;
			}
		}
		errno = 0;
		if ((sms.SMS[i].PDU == SMS_Status_Report) && mystrncasecmp(Config->deliveryreport, "log", 3)) {
			strcpy(buffer, DecodeUnicodeString(sms.SMS[i].Number));
			WriteSMSDLog("Delivery report: %s to %s", DecodeUnicodeString(sms.SMS[i].Text), buffer);
		} else {
			if (mystrncasecmp(Config->inboxformat, "detail", 0)) {
#ifdef GSM_ENABLE_BACKUP
				error = GSM_SaveSMSBackupFile(FullName, &backup);
#endif
				done = true;
			} else {
				file = fopen(FullName, "wb");
				if (file) {
					switch (sms.SMS[i].Coding) {
					case GSM_Coding_Unicode:
	    				case GSM_Coding_Default:
					    DecodeUnicode(sms.SMS[i].Text,buffer2);
					    if (mystrncasecmp(Config->inboxformat, "unicode", 0)) {
						buffer[0] = 0xFE;
	    					buffer[1] = 0xFF;
						fwrite(buffer,1,2,file);
						fwrite(sms.SMS[i].Text,1,strlen(buffer2)*2,file);
					    } else {
						fwrite(buffer2,1,strlen(buffer2),file);
					    }
					    break;
					case GSM_Coding_8bit:
					    fwrite(sms.SMS[i].Text,1,sms.SMS[i].Length,file);
					}
					fclose(file);
				} else error = GE_CANTOPENFILE;
			}
			if (error == GE_NONE) {
				WriteSMSDLog("%s %s", (sms.SMS[i].PDU == SMS_Status_Report?"Delivery report":"Received"), FileName);
			} else {
				WriteSMSDLog("Cannot save %s (%i)", FileName, errno);
				return GE_CANTOPENFILE;
			}
		}
	}
	return GE_NONE;
}

/* Find one multi SMS to sending and return it (or return GE_EMPTY)
 * There is also set ID for SMS
 */
static GSM_Error SMSDFiles_FindOutboxSMS(GSM_MultiSMSMessage *sms, GSM_SMSDConfig *Config, unsigned char *ID)
{
  	GSM_Error			error = GE_NOTSUPPORTED;
  	GSM_EncodeMultiPartSMSInfo	SMSInfo;
 	unsigned char 			FileName[100],FullName[400];
	unsigned char			Buffer[(GSM_MAX_SMS_LENGTH*MAX_MULTI_SMS+1)*2];
 	unsigned char			Buffer2[(GSM_MAX_SMS_LENGTH*MAX_MULTI_SMS+1)*2];
  	FILE				*File;
 	int				i, len, phlen;
 	char				*pos1, *pos2;
#if defined HAVE_DIRENT_H && defined HAVE_SCANDIR & defined HAVE_ALPHASORT
  	struct 				dirent **namelist = NULL;
  	int 				l, m ,n;

 	error = GE_NONE;
  	strcpy(FullName, Config->outboxpath);
  	FullName[strlen(Config->outboxpath)-1] = '\0';
  	n = scandir(FullName, &namelist, 0, alphasort);
  	m = 0;
 	while ((m < n) && ((*(namelist[m]->d_name) == '.') ||
 	                   !mystrncasecmp(namelist[m]->d_name,"out", 3) ||
 	                   ((strlen(namelist[m]->d_name) >= 4) &&
 	                     !mystrncasecmp(&namelist[m]->d_name[strlen(namelist[m]->d_name)-4],".txt",4)
 	                   )
 	                  )
 	      ) m++;
  	if (m < n) strcpy(FileName,namelist[m]->d_name);
  	for (l=0; l < n; l++) free(namelist[l]);
  	free(namelist);
  	namelist = NULL;
 	if (m >= n) error = GE_EMPTY;
#else
#ifdef WIN32
  	struct _finddata_t 		c_file;
  	long 				hFile;

  	strcpy(FullName, Config->outboxpath);
  	strcat(FullName, "OUT*.txt");
  	if((hFile = _findfirst( FullName, &c_file )) == -1L ) {
  		return GE_EMPTY;
  	} else {
  		strcpy(FileName,c_file.name);
  	}
  	_findclose( hFile );
#endif
#endif
 	if (error != GE_NONE) return error;

  	strcpy(FullName, Config->outboxpath);
  	strcat(FullName, FileName);

  	File = fopen(FullName, "rb");
 	len  = fread(Buffer, 1, sizeof(Buffer)-2, File);
  	fclose(File);
  	if (len<2) return GE_EMPTY;

 	if ((Buffer[0] != 0xFF || Buffer[1] != 0xFE) && (Buffer[0] != 0xFE || Buffer[1] != 0xFF)) {
 		if (len > GSM_MAX_SMS_LENGTH*MAX_MULTI_SMS) len = GSM_MAX_SMS_LENGTH*MAX_MULTI_SMS;
 		EncodeUnicode(Buffer2, Buffer, len);
 		len = len*2;
 		memmove(Buffer, Buffer2, len);
 	}

  	Buffer[len] 	= 0;
  	Buffer[len+1] 	= 0;
  	ReadUnicodeFile(Buffer2,Buffer);

  	SMSInfo.ReplaceMessage  = 0;
  	SMSInfo.Buffer		= Buffer2;
  	SMSInfo.Class		= -1;
 	if (mystrncasecmp(Config->transmitformat, "unicode", 0)) {
 		SMSInfo.ID = SMS_ConcatenatedTextLong;
 		SMSInfo.UnicodeCoding = true;
 	} else if (mystrncasecmp(Config->transmitformat, "7bit", 0)) {
 		SMSInfo.ID = SMS_ConcatenatedTextLong;
 		SMSInfo.UnicodeCoding = false;
 	} else {
		/* auto */
 		SMSInfo.ID = SMS_ConcatenatedAutoTextLong;
	}

  	GSM_EncodeMultiPartSMS(&SMSInfo,sms);

 	pos1 = FileName;
	strcpy(ID,FileName);
 	for (i = 1; i <= 3 && pos1 != NULL ; i++) pos1 = strchr(++pos1, '_');
 	if (pos1 != NULL) {
		/* OUT<priority><date>_<time>_<serialno>_<phone number>_<anything>.txt */
 		pos2 = strchr(++pos1, '_');
 		if (pos2 != NULL) {
 			phlen = strlen(pos1) - strlen(pos2);
 		} else {
			/* something wrong */
 			return GE_UNKNOWN;
		}
 	} else if (i == 2) {
		/* OUTxxxxxxx.txt or OUTxxxxxxx */
 		pos1 = &FileName[3]; pos2 = strchr(pos1, '.');
 		if (pos2 == NULL)
 			phlen = strlen(pos1);
 		else
 			phlen = strlen(pos1) - strlen(pos2);
	} else if (i == 4) {
		/* OUT<priority>_<phone number>_<serialno>.txt */
 		pos1 = strchr(FileName, '_');
 		pos2 = strchr(++pos1, '_');
 		phlen = strlen(pos1) - strlen(pos2);
 	} else {
		/* something wrong */
		return GE_UNKNOWN;
	}

 	for (len=0;len<sms->Number;len++) {
 		EncodeUnicode(sms->SMS[len].Number, pos1, phlen);
 	}

#ifdef DEBUG
	DecodeUnicode(sms->SMS[0].Number,Buffer);
 	dprintf("Found %i sms to \"%s\" with text \"%s\" cod %i lgt %i udh: t %i l %i\n",sms->Number, Buffer,
 			 DecodeUnicodeString(sms->SMS[0].Text), sms->SMS[0].Coding, sms->SMS[0].Length, sms->SMS[0].UDH.Type, sms->SMS[0].UDH.Length);
#endif
  	return GE_NONE;
  }

/* After sending SMS is moved to Sent Items or Error Items. */
static GSM_Error SMSDFiles_MoveSMS(unsigned char *sourcepath, unsigned char *destpath, unsigned char *ID, bool alwaysDelete)
{
	FILE 	*oFile,*iFile;
	int 	ilen = 0, olen = 0;
	char 	Buffer[(GSM_MAX_SMS_LENGTH*MAX_MULTI_SMS+1)*2],ifilename[400],ofilename[400];

	strcpy(ifilename, sourcepath);
	strcat(ifilename, ID);
	strcpy(ofilename, destpath);
	strcat(ofilename, ID);

#ifdef WIN32
	if (!mystrncasecmp(ifilename, ofilename, strlen(ofilename))) {
#else
	if (strcmp(ifilename, ofilename) != 0) {
#endif
		iFile = fopen(ifilename, "r");
		ilen = fread(Buffer, 1, sizeof(Buffer), iFile);
		fclose(iFile);
		oFile = fopen(ofilename, "w");
		olen = fwrite(Buffer, 1, ilen, oFile);
		fclose(oFile);
	}
	if (ilen == olen) {
		if ((strcmp(ifilename, "/") == 0) || (remove(ifilename) != 0)) {
			WriteSMSDLog("Could not delete %s (%i)", ifilename, errno);
			return GE_UNKNOWN;
		}
		return GE_NONE;
	} else {
		WriteSMSDLog("Error copying SMS %s -> %s", ifilename, ofilename);
		if (alwaysDelete) {
			if ((strcmp(ifilename, "/") == 0) || (remove(ifilename) != 0))
				WriteSMSDLog("Could not delete %s (%i)", ifilename, errno);
		}
		return GE_UNKNOWN;
	}
}

GSM_SMSDService SMSDFiles = {
	NONEFUNCTION,			/* Init */
	SMSDFiles_SaveInboxSMS,
	SMSDFiles_FindOutboxSMS,
	SMSDFiles_MoveSMS
};
