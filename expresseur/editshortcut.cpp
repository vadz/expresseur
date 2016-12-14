/////////////////////////////////////////////////////////////////////////////
// Name:        edishortcut.cpp
// Purpose:     modal dialog to edit the shortcut /  expresseur V3
// Author:      Franck REVOLLE
// Modified by:
// Created:     08/06/2015
// update : 23/11/2016 18:00
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
#include "wx/statline.h"
#include "wx/listctrl.h"
#include "wx/valgen.h"
#include "wx/listbox.h"
#include "wx/tokenzr.h"
#include "wx/config.h"

#include "global.h"
#include "luabass.h"
#include "basslua.h"
#include "editshortcut.h"

enum
{
	IDM_EDITSHORTCUT_LISTMIDI = ID_EDITSHORTCUT
}; 

wxBEGIN_EVENT_TABLE(editshortcut, wxDialog)
EVT_LISTBOX_DCLICK(IDM_EDITSHORTCUT_LISTMIDI, editshortcut::OnMidi )
wxEND_EVENT_TABLE()

editshortcut::editshortcut(wxWindow *parent, wxWindowID id, const wxString &title,
wxString *lname,
wxString *laction, wxArrayString nameAction,
wxString *lkey,
wxString *ldevice, wxArrayString nameDevice,
wxString *lchannel, wxArrayString nameChannel,
wxString *levent, wxArrayString nameEvent,
wxString *lmin, wxArrayString nameValueMin,
wxString *lmax, wxArrayString nameValueMax,
wxString *lparam ,
wxString *lstopOnMatch, wxArrayString nameStopOnMatch
)
: wxDialog(parent, id, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	wxSizerFlags sizerFlagMaximumPlace;
	sizerFlagMaximumPlace.Proportion(1);
	sizerFlagMaximumPlace.Expand();
	sizerFlagMaximumPlace.Border(wxALL, 2);

	wxSizerFlags sizerFlagMinimumPlace;
	sizerFlagMinimumPlace.Proportion(0);
	sizerFlagMinimumPlace.Border(wxALL, 5);

	wxBoxSizer *topsizer = new wxBoxSizer(wxVERTICAL);
	wxFlexGridSizer *secundsizer = new wxFlexGridSizer(2, wxSize(5, 5));
	secundsizer->AddGrowableCol(0);

	wxFlexGridSizer *fieldsizer = new wxFlexGridSizer(2, wxSize(5, 5));

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Name")), sizerFlagMinimumPlace);
	wxTextCtrl *fName = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, wxTextValidator(wxFILTER_EMPTY, lname));
	fName->SetToolTip(_("Any name. Free text"));
	fieldsizer->Add(fName, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Key shortcuts")), sizerFlagMinimumPlace);
	wxTextCtrl *fkey = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, wxTextValidator(wxFILTER_NONE, lkey));
	wxString s, s1;
	s1 = _("If max-value is specified, midi-msg will have data1 dispatched between min-value and max-value\nIf Max-value is not specified, midi-msg will have data1=min-value and data2 dispatched between 0 and 127");
	s.Printf("%s\n%s", _("Keystroke trigger"), s1);
	fkey->SetToolTip(s);
	fieldsizer->Add(fkey, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Device")), sizerFlagMinimumPlace);
	fTdevice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameDevice, 0, wxGenericValidator(ldevice));
	fTdevice->SetToolTip(_("Midiin device trigger"));
	fieldsizer->Add(fTdevice, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Channel")), sizerFlagMinimumPlace);
	fTchannel = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameChannel, 0, wxGenericValidator(lchannel));
	fTchannel->SetToolTip(_("Midiin channel trigger"));
	fieldsizer->Add(fTchannel, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Event")), sizerFlagMinimumPlace);
	fEvent = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameEvent, 0, wxGenericValidator(levent));
	fEvent->SetToolTip(_("Midiin event trigger"));
	fieldsizer->Add(fEvent, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Min Value")), sizerFlagMinimumPlace);
	fMin = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameValueMin, 0, wxGenericValidator(lmin));
	fMin->SetToolTip(_("Data1 of the midi-message.\nPitch of the note, control number, or program number"));
	fieldsizer->Add(fMin, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Max Value")), sizerFlagMinimumPlace);
	wxChoice *fMax = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameValueMax, 0, wxGenericValidator(lmax));
	s.Printf("%s\n%s", _("Maximum value (included ) for the pitch of the note, control number, or program number"), s1);
	fMax->SetToolTip(s);
	fieldsizer->Add(fMax, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Action")), sizerFlagMinimumPlace);
	wxChoice *fAction = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameAction, 0, wxGenericValidator(laction));
	fAction->SetToolTip(_("Action triggered"));
	fieldsizer->Add(fAction, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("On match")), sizerFlagMinimumPlace);
	wxChoice *fStopOnMatch = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nameStopOnMatch, 0, wxGenericValidator(lstopOnMatch));
	fStopOnMatch->SetToolTip(_("On match, continue or stop the analysis of next selectors"));
	fieldsizer->Add(fStopOnMatch, sizerFlagMaximumPlace);

	fieldsizer->Add(new wxStaticText(this, wxID_ANY, _("Parameter")), sizerFlagMinimumPlace);
	wxTextCtrl *fParameter = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, wxTextValidator(wxFILTER_NONE, lparam));
	fParameter->SetToolTip(_("Action parameter"));
	fieldsizer->Add(fParameter, sizerFlagMaximumPlace);

	wxBoxSizer *thirdsizer = new wxBoxSizer(wxVERTICAL);
	thirdsizer->Add(new wxStaticText(this, wxID_ANY, _("MIDI event detected")), sizerFlagMinimumPlace);
	listMidi = new wxListBox(this, IDM_EDITSHORTCUT_LISTMIDI);
	listMidi->SetToolTip(_("Double-click to copy"));
	listMidi->SetFont(wxFont(8, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
	listMidi->Clear();
	listMidi->Append(_("Valid Midi-In ports are opened."));
	listMidi->Append(_("MIDI events are displayed. Double-click to fill automatically the form."));

	thirdsizer->Add(listMidi, sizerFlagMaximumPlace);

	secundsizer->Add(fieldsizer, sizerFlagMaximumPlace);
	secundsizer->Add(thirdsizer, sizerFlagMaximumPlace);
	topsizer->Add(secundsizer, sizerFlagMaximumPlace);
	topsizer->Add(new wxStaticLine(this), sizerFlagMinimumPlace);
	topsizer->Add(CreateButtonSizer(wxOK | wxCANCEL), sizerFlagMinimumPlace);
	SetSizerAndFit(topsizer);
}
editshortcut::~editshortcut()
{
	//listMidi->Clear();
	basslua_getMidiinEvent(NULL);
}

void editshortcut::scanMidi()
{
	char midiEvent[MAXBUFCHAR];
	while (basslua_getMidiinEvent(midiEvent))
	{
		listMidi->Append(midiEvent);
		listMidi->SetFirstItem(listMidi->GetCount() - 1);
	}

}
void editshortcut::OnMidi(wxCommandEvent& event)
{
	// format of the text midi event is defined in basslua

	wxString smessage = event.GetString();
	wxStringTokenizer tokenizer(smessage, " =", wxTOKEN_STRTOK);
	int nb = tokenizer.CountTokens();
	if ( nb == 9 )
	{
		wxString token;
		
		//type msg
		token = tokenizer.GetNextToken();
		fEvent->SetStringSelection(token);

		//device
		token = tokenizer.GetNextToken();
		token = tokenizer.GetNextToken();
		long d; token.ToLong(&d);
		fTdevice->SetSelection(d);

		//channel
		token = tokenizer.GetNextToken();
		token = tokenizer.GetNextToken();
		long c; token.ToLong(&c);
		fTchannel->SetSelection(c);

		//data1
		token = tokenizer.GetNextToken();
		token = tokenizer.GetNextToken();
		fMin->SetStringSelection(token);

	}

}