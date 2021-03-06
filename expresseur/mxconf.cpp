/////////////////////////////////////////////////////////////////////////////
// Name:        mxconf.cpp
// Purpose:     to read and write the configuration /  expresseur V3
// Author:      Franck REVOLLE
// Modified by:
// Created:     27/07/2015
// update : 13/11/2016 22:00
// Copyright:   (c) Franck REVOLLE Expresseur
// Licence:    Expresseur licence
/////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
#pragma hdrstop
#endif

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif


#include "wx/dialog.h"
#include "wx/filename.h"
#include "wx/stattext.h"
#include "wx/sizer.h"
#include "wx/bitmap.h"
#include "wx/tglbtn.h"
#include "wx/spinctrl.h"
#include "wx/textfile.h"
#include "wx/listctrl.h"
#include "wx/valgen.h"
#include "wx/listbox.h"
#include "wx/tokenzr.h"
#include "wx/config.h"
#include "wx/dcclient.h"
#include "wx/msgdlg.h"
#include "wx/image.h"
#include "wx/filehistory.h"
#include "wx/hash.h"

#include "global.h"
#include "basslua.h"
#include "luabass.h"
#include "mxconf.h"

mxconf::mxconf()
{
	mConfig = new wxConfig(APP_NAME);
	mPrefix = "";
}
mxconf::~mxconf()
{
	delete mConfig;
}

wxConfig *mxconf::getConfig()
{
	return mConfig;
}

void mxconf::setPrefix()
{
	// the prefix is a checksum of all valid midiout's name
	char ch[MAXBUFCHAR] = "";
	wxString name;
	wxString names;
	wxString spipe;
	int nbMidioutDevice = 0;
	int checksum = 0;
	while (true)
	{
		basslua_call(moduleLuabass, soutGetMidiName, "i>s", nbMidioutDevice + 1, ch);
		if ((*ch == '\0') || (nbMidioutDevice >= MAX_MIDIOUT_DEVICE))
			break;
		nbMidioutDevice++;
		bool valid = false;
		basslua_call(moduleGlobal, soutMidiIsValid, "s>b", ch, &valid);
		if (valid)
		{
			char *pt = ch;
			if ((strlen(ch) > 4) && (strncmp(ch + 1, "- ", 2) == 0))
				pt = ch + 3; // suppress the prefix "X- "
			if (names.Length() < 128 )
				names += spipe + wxString(pt);
			spipe = "|";
			while (*pt != '\0')
			{
				checksum += *pt;
				pt++;
			}
		}
	}
	if (nbMidioutDevice == 0)
		names = "no midi-out device";
	checksum = checksum % 1024;
	wxString keyPrefix;
	keyPrefix.Printf("%s/%d", CONFIG_HARDWARE , checksum);
	wxString prefixConfig = mConfig->Read(keyPrefix, "");
	if (prefixConfig.IsEmpty())
	{
		mConfig->SetPath(CONFIG_HARDWARE);
		int nbGroups = mConfig->GetNumberOfGroups();
		if (nbGroups > 0)
		{
			if (wxMessageBox("The hardware midi-out configuration is a new one.\nDo you want to create a new setting of the mixer from scratch ? ",
				"New hardware midi-out", wxYES_NO) == wxNO)
			{
				// get the list of existing configs already available
				wxArrayString listConfig;
				// first enum all entries
				long dummy;
				wxString mgroup;
				bool bCont = mConfig->GetFirstGroup(mgroup, dummy);
				while (bCont)
				{
					listConfig.Add(mgroup);
					bCont = mConfig->GetNextGroup(mgroup, dummy);
				}
				prefixConfig = wxGetSingleChoice("Select a midi-out configuration to reuse for the mixer",
					"Midi-out mixer setting", listConfig, 0);
			}
			if (prefixConfig.IsEmpty())
			{
				prefixConfig = wxGetTextFromUser("Name of this new midi-out configuration", "Midi-out mixer setting", names);
			}
		}
		if (prefixConfig.IsEmpty())
		{
			prefixConfig = names ;
		}
		prefixConfig = prefixConfig.ToAscii();
		prefixConfig.Replace("/", "_", true);
		mConfig->Write(keyPrefix, prefixConfig);
		mConfig->SetPath("/");
	}

	mPrefix.Printf("%s/%s/", CONFIG_HARDWARE, prefixConfig);
}

long mxconf::writeFile(wxTextFile *lfile, wxString key, long defaultvalue, bool prefix, wxString name)
{
	wxString s, v;
	long l = this->get(key, defaultvalue, prefix, name);
	s.Printf("%s=%d", prefixKey( key , false, name), l);
	lfile->AddLine(s);
	return l;
}
wxString mxconf::writeFile(wxTextFile *lfile, wxString key, wxString defaultvalue, bool prefix , wxString name)
{
	wxString s;
	wxString l = this->get(key, defaultvalue, prefix, name);
	s.Printf("%s=%s", prefixKey(key, false, name), l);
	lfile->AddLine(s);
	return l;
}

wxString mxconf::readFileLines(wxTextFile *lfile, wxString key)
{
	wxString str;
	for (str = lfile->GetFirstLine(); !lfile->Eof(); str = lfile->GetNextLine())
	{
		if (str.StartsWith(key))
			return str;
	}
	return wxEmptyString;
}
wxString mxconf::readFile(wxTextFile *lfile, wxString key, wxString defaultvalue, bool prefix, wxString name)
{
	wxString s, s2;
	s = readFileLines(lfile, prefixKey(key, false, name));
	if (s.IsEmpty())
		return get(key,defaultvalue,prefix,name);
	if (!s.StartsWith(prefixKey(key, false, name)))
		s2 = defaultvalue;
	else
		s2 = s.AfterFirst('=');
	this->set(key, s2, prefix , name);
	return s2;
}
long mxconf::readFile(wxTextFile *lfile, wxString key, long defaultvalue, bool prefix, wxString name)
{
	wxString s, s2;
	long l;
	s = readFileLines(lfile, prefixKey(key, false, name));
	if (s.IsEmpty())
		return get(key, defaultvalue, prefix, name);
	if (!s.StartsWith(prefixKey(key, false, name)))
		l = defaultvalue;
	else
	{
		s2 = s.AfterFirst('=');
		s2.ToLong(&l);
	}
	this->set(key, l, prefix , name);
	return(l);
}

wxString mxconf::get(wxString key, wxString defaultvalue, bool prefix, wxString name)
{
	wxString s;
	mConfig->Read(prefixKey(key, prefix,name), &s, defaultvalue);
	mConfig->Write(prefixKey(key, prefix,name), s);
	return s;
}
long mxconf::get(wxString key, long defaultvalue, bool prefix, wxString name)
{
	long l;
	mConfig->Read(prefixKey(key, prefix, name), &l, defaultvalue);
	mConfig->Write(prefixKey(key, prefix, name), l);
	return(l);
}

void mxconf::set(wxString key, wxString s, bool prefix, wxString name)
{
	mConfig->Write(prefixKey(key, prefix, name), s);
}
void mxconf::set(wxString key, long l, bool prefix, wxString name)
{
	mConfig->Write(prefixKey(key, prefix, name), l);
}

void mxconf::remove(wxString key, bool prefix, wxString name)
{
	mConfig->DeleteEntry(prefixKey(key, prefix = false, name));
}
bool mxconf::exists(wxString key, bool prefix, wxString name)
{
	return (mConfig->Exists(prefixKey(key, prefix, name)));
}

wxString mxconf::prefixKey(wxString key, bool prefix, wxString name)
{
	wxString s1;
	if (name.IsEmpty())
		s1.Printf("%s%s", prefix ? mPrefix : "", key);
	else
		s1.Printf("%s%s/%s", prefix ? mPrefix : "", key, name);
	return s1;
}