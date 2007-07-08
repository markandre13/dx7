/*
 *  DX7 Patch Editor
 *  Copyright (C) 2006 Mark-André Hopf <mhopf@mark13.org>
 *
 *  DX7 Envelope Tables are taken from legasynth-0.4.1
 *  Copyright (C) 2002 Juan Linietsky <coding@reduz.com.ar>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef DARWIN
#include <CoreFoundation/CFSocket.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#endif

#include <iostream>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "thread.hh"

#include <toad/toad.hh>
#include <toad/dialog.hh>
#include <toad/textfield.hh>
#include <toad/integermodel.hh>
#include <toad/pushbutton.hh>
#include <toad/figure.hh>
#include <toad/ioobserver.hh>
#include <toad/filedialog.hh>
#include <toad/combobox.hh>
#include <toad/stl/vector.hh>

#include <fstream>
#include <algorithm>

using namespace toad;

extern float EG_rate_rise_duration[128];
extern float EG_rate_decay_duration[128];
extern float EG_rate_decay_percent[128];
extern float EG_rate_rise_percent[128];

#ifdef DARWIN
class TMIDIInterface:
  public TThread
{
    static int fds[2];
    CFSocketRef sr;
    CFRunLoopSourceRef rlsr;
    MIDIClientRef client;
    MIDIPortRef inPort;
    static MIDIPortRef outPort;
    static MIDIEndpointRef dst;

    static void midiRead(const MIDIPacketList *, void *, void *);
    static void socketRead(CFSocketRef, CFSocketCallBackType, CFDataRef, const void *, void *);
  public:
    TMIDIInterface();
    int getFD() const {
      return fds[1];
    }
    struct TPort {
      TPort(MIDIEndpointRef r, const string &n): ref(r), name(n) {}
      MIDIEndpointRef ref;
      string name;
    };
    typedef GVector<TPort> TPortList;
    TPortList in, out;
    void main();
};

class TPortListAdapter:
  public TTableAdapter, GModelOwner<TMIDIInterface::TPortList>
{
    TCoord w, h;
  public:
    TPortListAdapter(TMIDIInterface::TPortList *m) { setModel(m); }
    ~TPortListAdapter() { setModel(0); }
    TMIDIInterface::TPortList* getModel() const { return GModelOwner<TMIDIInterface::TPortList>::getModel(); }
    void modelChanged(bool newmodel) {
      if (model) {
        TFont &font(TOADBase::getDefaultFont());
        h = font.getHeight();
        w = 0;
        for(TMIDIInterface::TPortList::const_iterator p = model->begin(); p != model->end(); ++p)
          w = max(w, font.getTextWidth((*p).name));
      }
      TTableAdapter::modelChanged(newmodel);
    }
    size_t getCols() { return 1; }
    void tableEvent(TTableEvent &te);
};

void
TPortListAdapter::tableEvent(TTableEvent &te)
{
  switch(te.type) {
    case TTableEvent::GET_COL_SIZE:
      te.w = w+2;
       break;
    case TTableEvent::GET_ROW_SIZE:
      te.h = h+2;
      break;
    case TTableEvent::PAINT:
      renderBackground(te); 
      te.pen->drawString(1,1, (*model)[te.row].name);
      renderCursor(te);
      break;
  }
}  
#endif

int fd;
class TMIDIValue;
TMIDIValue* mv[158];
vector<TFigure*> algfig;

class TMIDIValue:
  public TIntegerModel
{
    unsigned g, h, p;
    string name;
  public:
    TMIDIValue(unsigned g, unsigned h, unsigned p, int min, int max, int def) {
      this->g = g;
      this->h = h;
      this->p = p;
      setRangeProperties(def,0,min,max);
      mv[(h<<7)+p] = this;
    }
  protected:
    void changed();
};

#ifdef DARWIN
int TMIDIInterface::fds[2];
MIDIPortRef TMIDIInterface::outPort;
MIDIEndpointRef TMIDIInterface::dst;

TMIDIInterface::TPort
ref2port(MIDIEndpointRef ref)
{
  CFStringRef pname, pmanuf, pmodel;
  char name[64], manuf[64], model[64];
  MIDIObjectGetStringProperty(ref, kMIDIPropertyName, &pname);
  MIDIObjectGetStringProperty(ref, kMIDIPropertyManufacturer, &pmanuf);
  MIDIObjectGetStringProperty(ref, kMIDIPropertyModel, &pmodel);
    
  CFStringGetCString(pname, name, sizeof(name), 0);
  CFStringGetCString(pmanuf, manuf, sizeof(manuf), 0);
  CFStringGetCString(pmodel, model, sizeof(model), 0);
  CFRelease(pname);
  CFRelease(pmanuf);
  CFRelease(pmodel);
  printf("SRC: name=%s, manuf=%s, model=%s\n", name, manuf, model);
  string nm; nm+=model; nm+=" "; nm+=name;
  return TMIDIInterface::TPort(ref, nm);
}

TMIDIInterface::TMIDIInterface()
{
}

void
TMIDIInterface::main()
{
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)!=0) {
    perror("socketpair");
    return;
  }

  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  fcntl(fds[1], F_SETFL, O_NONBLOCK);
  
  sr = CFSocketCreateWithNative(
    0,
    fds[0],
    kCFSocketReadCallBack,
    &socketRead,
    0
  );
  assert(sr);
  
  rlsr = CFSocketCreateRunLoopSource(
    0,
    sr,
    0
  );
  
  CFRunLoopAddSource(
    CFRunLoopGetCurrent(),
    rlsr,
    kCFRunLoopCommonModes
  );

#if 1
  for(int i=0; i<MIDIGetNumberOfSources(); ++i) {
    MIDIEndpointRef ref = MIDIGetSource(i);
    if (!ref)
      continue;
    in.push_back(::ref2port(ref));
  }
  for(int i=0; i<MIDIGetNumberOfDestinations(); ++i) {
    MIDIEndpointRef ref = MIDIGetDestination(i);
    if (!ref)
      continue;
    out.push_back(::ref2port(ref));
  }
#else
cout << "number of MIDI devices: " << MIDIGetNumberOfDevices() << endl;
cout << "number of MIDI sources: " << MIDIGetNumberOfSources() << endl;
cout << "number of MIDI destinations: " << MIDIGetNumberOfDestinations() << endl;
// MIDIGetSource, MIDIGetDestination
  for(int i=0; i<MIDIGetNumberOfDevices(); ++i) {
    MIDIDeviceRef dev = MIDIGetDevice(i);
    
    CFStringRef pname, pmanuf, pmodel;
    char name[64], manuf[64], model[64];
    MIDIObjectGetStringProperty(dev, kMIDIPropertyName, &pname);
    MIDIObjectGetStringProperty(dev, kMIDIPropertyManufacturer, &pmanuf);
    MIDIObjectGetStringProperty(dev, kMIDIPropertyModel, &pmodel);
    
    CFStringGetCString(pname, name, sizeof(name), 0);
    CFStringGetCString(pmanuf, manuf, sizeof(manuf), 0);
    CFStringGetCString(pmodel, model, sizeof(model), 0);
    CFRelease(pname);
    CFRelease(pmanuf);
    CFRelease(pmodel);
    printf("name=%s, manuf=%s, model=%s\n", name, manuf, model);
    for(int j=0; j<MIDIDeviceGetNumberOfEntities(dev); ++j) {
      MIDIEntityRef ent = MIDIDeviceGetEntity(dev, j);
      
      char ename[64];
      CFStringRef str = 0;
      MIDIObjectGetStringProperty(ent, kMIDIPropertyName, &str);
      if (str) {
        CFStringGetCString(str, ename, sizeof(ename), 0);
        CFRelease(str);
      } else {
        ename[0] = 0;
      }
      
      printf("  entity %i: destinations=%i, sources=%i, name='%s'\n",
        j,
        MIDIEntityGetNumberOfDestinations(ent),
        MIDIEntityGetNumberOfSources(ent),
        ename);
        
        // MIDIEndPointRef src = MIDIEntityGetSource(ent, k);
    }
  }
#endif
  client = 0;
  MIDIClientCreate(CFSTR("DX7 Client"),
                   0,
                   0,
                   &client);
  
  inPort = 0;
  MIDIInputPortCreate(client,
                      CFSTR("DX7 In"),
                      &midiRead,
                      0,
                      &inPort);
                      
  MIDIEndpointRef src = MIDIGetSource(0);

  MIDIPortConnectSource(inPort, src, NULL);

  outPort = 0;
  MIDIOutputPortCreate(client,
                      CFSTR("DX7 Out"),
                      &outPort);
  dst = MIDIGetDestination(0);
  CFRunLoopRun();
}

void 
TMIDIInterface::midiRead(const MIDIPacketList *pktlist, void *readProcRefCon,C>packet[i].length);
  }
}

void
TMIDIInterface::socketRead(
   CFSocketRef s, 
   CFSocketCallBackType callbackType, 
   CFDataRef address, 
   const void *data, 
   void *info)
{
  MIDIPacketList list;
  list.numPackets = 1;
  list.packet[0].timeStamp = 0;
  while(true) {
    ssize_t n = read(fds[0], list.packet[0].data, 256);
    if (n<0 && errno!=EAGAIN) {
      perror("read");
    }
    if (n<=0)
      break;
    list.packet[0].length = n;
//    cout << "send " << n << " bytes" << endl;
    MIDISend(outPort, dst, &list);
  }
}
#endif

class TDX7Editor:
  public TDialog
{
    TMIDIInterface &midi;
  public:
    TDX7Editor(TWindow *parent, const string &title, TMIDIInterface &midi);
  protected:
    void dumpRequest(int);
    void paint();
    void sendPacked32();
};

void
TMIDIValue::changed()
{
  if (!fd)
    return;
  
  unsigned char b[7];
  b[0] = 0xF0;
  b[1] = 0x43;
  b[2] = 0x10;
  b[3] = (g << 2 | h);
  b[4] = p;
  b[5] = value;
  b[6] = 0xf7;
  write(fd, b, 7);
}

// g h p
struct vced_t {
  unsigned g;
  unsigned h;
  unsigned p;
  const char *name;
  unsigned min;
  unsigned max;
  unsigned def;
};
vced_t vced[] = {
  {  0,  0,   0,  "r1", 0, 99, 99 }, // 0
  {  0,  0,   1,  "r2", 0, 99, 99 },
  {  0,  0,   2,  "r3", 0, 99, 99 },
  {  0,  0,   3,  "r4", 0, 99, 99 },
  {  0,  0,   4,  "l1", 0, 99, 99 },
  {  0,  0,   5,  "l2", 0, 99, 99 },
  {  0,  0,   6,  "l3", 0, 99, 99 },
  {  0,  0,   7,  "l4", 0, 99,  0 },
  {  0,  0,   8,  "bp", 0, 99, 39 },
  {  0,  0,   9,  "ld", 0, 99,  0 },
  {  0,  0,  10,  "rd", 0, 99,  0 },
  {  0,  0,  11,  "lc", 0,  3,  0 },
  {  0,  0,  12,  "rc", 0,  3,  0 },
  {  0,  0,  13,  "rs", 0,  7,  0 },
  {  0,  0,  14, "ams", 0,  3,  0 },
  {  0,  0,  15,  "ts", 0,  7,  0 },
  {  0,  0,  16,  "tl", 0, 99,  0 },
  {  0,  0,  17,  "pm", 0,  1,  0 },
  {  0,  0,  18,  "pc", 0, 31,  1 },
  {  0,  0,  19,  "pf", 0, 99,  0 },
  {  0,  0,  20,  "pd", 0, 14,  7 },
  
  {  0,  0, 126, "pr1", 0, 99, 99 }, // 21
  {  0,  0, 127, "pr2", 0, 99, 99 },
  {  0,  1,   0, "pr3", 0, 99, 99 },
  {  0,  1,   1, "pr4", 0, 99, 99 },
  {  0,  1,   2, "pl1", 0, 99, 50 },
  {  0,  1,   3, "pl2", 0, 99, 50 },
  {  0,  1,   4, "pl3", 0, 99, 50 },
  {  0,  1,   5, "pl4", 0, 99, 50 },
  {  0,  1,   6, "als", 0, 31,  0 },
  {  0,  1,   7, "fbl", 0,  7,  0 },
  {  0,  1,   8, "opi", 0,  7,  0 },
  {  0,  1,   9, "lfs", 0,  7,  0 },
  {  0,  1,  10, "lfd", 0,  7,  0 },
  {  0,  1,  11,"lpmd", 0,  7,  0 },
  {  0,  1,  12,"lamd", 0,  7,  0 },
  {  0,  1,  13,"lfks", 0,  7,  0 },
  {  0,  1,  14, "lfw", 0,  7,  0 },
  {  0,  1,  15,"lpms", 0,  7,  0 },
  {  0,  1,  16,"trnp", 0,  7,  0 }, 
};

double
getDuration(int p_rate, int p_level_l, int p_level_r) {
  float *duration_table=(p_level_r>p_level_l) ? EG_rate_rise_duration : EG_rate_decay_duration;
  double duration=duration_table[p_rate];

  float* percent_table =(p_level_r>p_level_l) ? EG_rate_rise_percent  : EG_rate_decay_percent;
  duration *= fabs(percent_table[p_level_r]-percent_table[p_level_l]);
  return duration;
}

class TDX7Envelope:
  public TWindow
{
  public:
    TDX7Envelope(TWindow *parent, const string &title);
    void paint();
};

TDX7Envelope::TDX7Envelope(TWindow *parent, const string &title):
  TWindow(parent, title)
{
  setBackground(0,0,0);
  for(int op=0; op<6; ++op) {
    int j=op*21;
    for(int i=0; i<8; ++i) {
      CONNECT(mv[j+i]->sigChanged, this, invalidateWindow, true);
    }
  }
}

void
TDX7Envelope::paint()
{
  TPen pen(this);

  int h = getHeight();
  
  static const TRGB rgb[6] = {
    TRGB( 209, 163, 204 ),
    TRGB( 249, 130, 142 ),
    TRGB( 255, 153,  71 ),
    TRGB( 255, 232,  22 ),
    TRGB(  94, 224,  68 ),
    TRGB(   0, 170, 209 )
  };

  double d[6][4];
  double keyoff = 0.0;
  double release = 0.0;
  for(int op=0; op<6; ++op) {
    int i = op * 21;
    d[op][0] = getDuration(*mv[i+0], *mv[i+7], *mv[i+4]);
    d[op][1] = getDuration(*mv[i+1], *mv[i+4], *mv[i+5]);
    d[op][2] = getDuration(*mv[i+2], *mv[i+5], *mv[i+6]);

    double ko = 0.0;
    for(int j=0; j<3; ++j)
      ko += d[op][j];
    if (ko>keyoff)
      keyoff=ko;
    
    d[op][3] = getDuration(*mv[i+3], *mv[i+6], *mv[i+7]);
    if (d[op][3]>release)
      release = d[op][3];
  }
  keyoff += 10.0;
  double w = getWidth() / (keyoff + release);
  
  pen.setColor(30,30,30);
//  pen.drawLine(keyoff*w, 0, keyoff*w, getHeight());
  pen.fillRectangle(keyoff*w,0,getWidth(),getHeight());

  for(int op=0; op<6; ++op) {
    int i = op * 21;
    pen.setColor(rgb[op]);
    pen.moveTo( 0, 
                h - h / 99.0 * *mv[i+7]);
    pen.lineTo( d[op][0]*w,
                h - h / 99.0 * *mv[i+4]);
    pen.lineTo((d[op][0]+d[op][1])*w,
                h - h / 99.0 * *mv[i+5]);
    pen.lineTo((d[op][0]+d[op][1]+d[op][2])*w,
                h - h / 99.0 * *mv[i+6]);
    pen.lineTo(keyoff*w,
                h - h / 99.0 * *mv[i+6]);
    pen.lineTo((d[op][0]+d[op][1]+d[op][2]+keyoff+d[op][3])*w,
                h - h / 99.0 * *mv[i+7]);
  }
}

TDX7Editor::TDX7Editor(TWindow *parent, const string &title, TMIDIInterface &m):
  TDialog(parent, title), midi(m)
{
  for(unsigned i=0; i<6; ++i) {
    for(unsigned j=0; j<21; ++j) {
      char buffer[80];
      snprintf(buffer, sizeof(buffer), "op%u%s", i, vced[j].name);
      new TTextField(this, buffer,
        new TMIDIValue(vced[j].g,
                       vced[j].h,
                       vced[j].p+i*21,
                       vced[j].min,
                       vced[j].max,
                       vced[j].def));
    }
  }
  for(unsigned i=21; i<40; ++i) {
    new TTextField(this, vced[i].name,
      new TMIDIValue(vced[i].g,
                     vced[i].h,
                     vced[i].p,
                     vced[i].min,
                     vced[i].max,
                     vced[i].def));
  }

  new TDX7Envelope(this, "envelope");

  TPushButton *pb;
  pb = new TPushButton(this, "dump");
  CONNECT(pb->sigClicked, this, dumpRequest, 0);

  pb = new TPushButton(this, "packed32");
  CONNECT(pb->sigClicked, this, dumpRequest, 9);

  pb = new TPushButton(this, "snd-packed32");
  CONNECT(pb->sigClicked, this, sendPacked32);

  TComboBox *cb;
  cb = new TComboBox(this, "midi-in");
  cb->setAdapter(new TPortListAdapter(&midi.in));
  cb = new TComboBox(this, "midi-out");
  cb->setAdapter(new TPortListAdapter(&midi.out));

  CONNECT(mv[134]->sigChanged, this, invalidateWindow, true);

  loadLayout("TDX7Editor.atv");
}

void
TDX7Editor::paint()
{
//  TDialog::paint();
  TPen pen(this);
  
  TFigure *f = algfig[mv[134]->getValue()];
// cout << "paint alg " << mv[134]->getValue() << endl;
  pen.translate(222,0);
  if (f->mat)
    pen.multiply(f->mat);
  f->paint(pen, TFigure::NORMAL);
}

class TDX7FileDialog:
  public TFileDialog
{
  public:
    TDX7FileDialog(TWindow *parent, const string &title, EMode mode = MODE_OPEN);
  protected:
    void update();
    void paint();
    
    TRectangle cnt;
    bool empty;
    string voice[32];
};

TDX7FileDialog::TDX7FileDialog(TWindow *parent, const string &title, EMode mode):
  TFileDialog(parent, title, mode)
{
  setLayout(0);
  cnt.set(5,getHeight(),getWidth()-10,155);
  setSize(getWidth(), getHeight()+160);
  connect(entrychoice.sigChanged, this, &TDX7FileDialog::update);
}

void
TDX7FileDialog::update()
{
  empty = true;
  if (entrychoice.getRow()<entries.getRows()) {
    const TDirectory::TDirectoryEntry &file = entries[entrychoice.getRow()];
    string filename = getFilename();
    if (!S_ISDIR(file.mode)) {
      cout << "file: " << filename << endl;
      ifstream in(filename.c_str());
      if (in) {
        byte data[4104];
        in.read((char*)data, 4104);
        if (in) {
          cout << "read file" << endl;
          if (data[0] == 0xf0 &&
              data[1] == 0x43 &&
              data[3] == 9 &&
              data[4] == 0x20 &&
              data[5] == 0x00)
          {
            for (int i=0; i<32; ++i) {
              voice[i].assign((char*)data+i*0x80+0x7c, 10);
//              cout << "voice " << i << ": " << voice[i] << endl;
            }
            empty = false;
          } else {
            cout << "not voice file" << endl;
          }
        }
      }
    }
  }
  invalidateWindow(cnt);
}

void
TDX7FileDialog::paint()
{
  TPen pen(this);
  pen.draw3DRectangle(cnt);
  if (empty)
    return;
  int i=0;
  for(int x=0; x<4; ++x) {
    for(int y=0; y<8; ++y) {
      pen.drawString(cnt.x+x*cnt.w/4,cnt.y + y*pen.getHeight(), voice[i++]);
    }
  }
}

void
TDX7Editor::sendPacked32()
{
  TDX7FileDialog dlg(0, "Send Packed 32 Voice..");
  dlg.addFileFilter("SysEx (*.syx)");
  dlg.doModalLoop();
  if (dlg.getResult()!=TMessageBox::OK)
    return;
  
  string data;
  ifstream in(dlg.getFilename().c_str());
  while(in) {
    data += in.get();
  }
  cout << "loaded file " << data.size() << endl;
  
  for (int i=0; i<32; ++i) {
    cout << i << ": ";
      for(int j=0; j<10; ++j) {
        cout << data[i*0x80+j+0x7c];
      }
      cout << endl;
  }
  
  if (data.size()==4105)
    write(fd, data.c_str(), data.size()-1);
}

void
TDX7Editor::dumpRequest(int format)
{
  // dump request
  unsigned char cmd[5];
  cmd[0] = 0xF0;
  cmd[1] = 0x43;
  cmd[2] = 0x20;
  cmd[3] = format;
  cmd[4] = 0xf7;
  write(fd, cmd, 5);
//  cout << "wrote dump request" << endl;
}

static void load();

class TMIDIListener:
  public TIOObserver
{
  public:
    TMIDIListener(int fd): TIOObserver(fd) {
      state = 0;
    }
  protected:
    void canRead();

    unsigned state;
    unsigned cntr;
    unsigned len;
    byte chk;
  
    vector<byte> sysex;
    
    void voice();
    void packed32();
};

void TMIDIListener::canRead()
{
  while(true) {
    char data[256];
    ssize_t n = read(fd(), data, sizeof(data));
    if (n<0 && errno!=EAGAIN) {
      perror("read");
    }
    if (n<=0)
      break;
//    cout << "goto MIDI of " << n << " bytes" << endl;

    for(ssize_t j=0; j<n; ++j) {
      unsigned char b = data[j];
      // printf("(%u)%02x ", state, b);
      switch(state) {
        case 0: // status message
          if (b==0xF0) {
            sysex.clear();
            sysex.push_back(b);
            state = 1;
          }
          break;
        case 1: // identification (0x43 Yamaha, 0x41 Roland, ...)
          if (b==0x43) {
            sysex.push_back(b);
            state = 2;
          } else {
            cout << "SysEx isn't Yamaha" << endl;
            sysex.clear();
            state = 0;
          }
          break;
        case 2: // substatus/device number
          if (b==0x00) {
            sysex.push_back(b);
            state = 3;
          } else {
            switch(b & 0xf0) {
              case 0x10:
                cout << "ignoring parameter change" << endl;
                break;
              case 0x20:
                cout << "ignoring dump request" << endl;
                break;
              default:
                cout << "Substatus/Device Number isn't 0 (it's " << (unsigned)b << ", might be my error)" << endl;
            }
            sysex.clear();
            state = 0;
          }
          break;
        case 3: // format number
          if (b==0xF7) {
            cout << "unexpected end of SysEx" << endl;
            state = 0;
          } else {
            sysex.push_back(b);
            state = 4;
          }
          break;
        case 4: // msb
          if (b==0xF7) {
            cout << "unexpected end of SysEx" << endl;
            state = 0;
          } else {
            sysex.push_back(b);
            state = 5;
            len = b << 7;
          }
          break;
        case 5: // lsb
          if (b==0xF7) {
            cout << "unexpected end of SysEx" << endl;
            state = 0;
          } else {
            sysex.push_back(b);
            cntr = 0;
            state = 6;
            len |= b;
            chk = 0;
            // cout << "dumb begins with " << len << " bytes" << endl;
          }
          break;
        case 6:
          if (b==0xF7) {
            cout << "unexpected end of SysEx" << endl;
            state = 0;
          } else {
            sysex.push_back(b);
            ++cntr;
            chk = (chk + b) & 0x7F;
            if (cntr==len) {
              state = 7;
            }
          }
          break;
        case 7:
          if (b==0xF7) {
            state = 0;
          } else {
            sysex.push_back(b);
            chk ^= 0x7F;
            chk++;
            if (chk==b) {
              state = 8;
            } else {
              cout << "SysEx error" << endl;
              state = 0;
            }
          }
          break;
        case 8:
          if (b==0xF7) {
            sysex.push_back(b);
            cout << "SysEx successfully received" << endl;
            switch(sysex[3]) {
              case 0:
                cout << "VOICE EDIT BUFFER" << endl;
                voice();
                break;
              case 9:
                cout << "PACKED 32 VOICE" << endl;
                packed32();
                break;
            }
            sysex.clear();
          }
          state = 0;
          break;
      }
    }
  }
  // cout << endl;
}

void
TMIDIListener::voice()
{
  if (sysex.size()!=163) {
    cout << "VOICE EDIT BUFFER SysEx has wrong size" << endl;
    return;
  }
  int ofd = ::fd;
  ::fd = 0;

  string name;
  for(int i=0; i<155; ++i) {
    byte b = sysex[i+6];
    if (mv[i]) {
      mv[i]->setValue(b);
    }
    if (i>=145 && i<=154)
      name += b;
  }
  cout << "loaded " << name << endl;
  ::fd = ofd;
}

void
TMIDIListener::packed32()
{
  cout << "received " << sysex.size() << " bytes of packed 32" << endl;
  if (sysex.size()!=4104) {
    cout << "PACKED 32 VOICE SysEx has wrong size" << endl;
    return;
  }
  
  TFileDialog dlg(0, "Save Packed 32 Voice SysEx..", TFileDialog::MODE_SAVE);
  dlg.addFileFilter("SyEx (*.syx)");
  dlg.doModalLoop();
  if (dlg.getResult()==TMessageBox::OK) {
    ofstream out(dlg.getFilename().c_str());
    for(vector<byte>::iterator p = sysex.begin();
        p != sysex.end();
        ++p)
    {
      out.put((byte)*p);
    }
  }
}

int
main(int argc, char **argv, char **envv)
{
#ifdef DARWIN
  TMIDIInterface midi;
  midi.start();
#else
  const char *devname = "/dev/midi00";
  if (argc>1)
    devname = argv[1];
  fd = open(devname, O_RDWR);
  if (fd==-1) {
    fprintf(stderr, "Error: failed to open raw midi device '%s': %s\n"
                    "Please start the program with: %s <path to your raw MIDI device>\n" ,
            devname, strerror(errno), argv[0]);
//    return 1;
  }
#endif
  for(int i=0; i<158; ++i)
    mv[i] = 0;

  toad::initialize(argc, argv, envv); {
    load();
#ifdef DARWIN
    fd = 0;
    new TMIDIListener(midi.getFD());
#else
    new TMIDIListener(fd);
#endif
    TDX7Editor wnd(0, "DX7", midi);
#ifdef DARWIN
    fd = midi.getFD();
#endif
    toad::mainLoop();
  } toad::terminate();
  return 0;
}

/**
 * load algorithm images
 */
void
load()
{
  // new variant vector icons
  ifstream fin("alg.fish");
  
  string iconname;
  TFGroup *g = 0;
  
  if (fin) {
    // TATVParser in(&fin);
    TInObjectStream in(&fin);
    in.setInterpreter(0);
    while(in.parse()) {
#if 0
      for(unsigned i=0; i<in.getDepth(); ++i) {
        cout << "  ";
      }
      cout << in.getWhatName()
           << ": ('" << in.attribute 
           << "', '" << in.type
           << "', '" << in.value
           << "'), position=" << in.getPosition()
           << ", depth=" << in.getDepth() << endl;
#endif
      bool ok = false;
      switch(in.getDepth()) {
        case 0:
          switch(in.what) {
            case ATV_GROUP:
              if (in.type=="fischland::TDocument")
                ok=true;
              break;
            case ATV_VALUE:
            case ATV_FINISHED:
              ok = true;
              break;
          }
          break;
        case 1:
          switch(in.what) {
            case ATV_GROUP:
              if (in.type=="fischland::TSlide") {
                if (!g) {
                  g = new TFGroup;
                  g->mat = new TMatrix2D();
                  g->mat->scale(1.0/96.0, 1.0/96.0);
//                  g->mat->translate(48.0, 48.0);
                }
                ok = true;
            } break;
            case ATV_VALUE:
              if (in.attribute == "name") {
                iconname = in.value;
// cout << "load icon '" << iconname << "'" << endl;
              }
              ok = true;
              break;
            case ATV_FINISHED: {
              if (g) {
                g->calcSize();
                algfig.push_back(g);
                g = 0;
              }
              ok = true;
            } break;
          }
          break;
        case 2:
          switch(in.what) {
            case ATV_GROUP:
              if (in.type=="fischland::TLayer")
                ok = true;
              break;
            case ATV_VALUE:
            case ATV_FINISHED:
              ok = true;
              break;
          }
          break;
        case 3:
          switch(in.what) {
            case ATV_GROUP: {
              TObjectStore &os(getDefaultStore());
              TSerializable *s = os.clone(in.type);
              if (s) {
//                cout << "parse object" << endl;
                in.setInterpreter(s);
in.stop();
                ok = in.parse();
//                cout << "parsed object" << endl;
                if (!ok) {
                  cout << "not okay: " << in.getErrorText() << endl;
                }
                in.setInterpreter(0);
                
                TFigure *nf = dynamic_cast<TFigure*>(s);
                if (!nf)
                  delete nf;
                else {
                  g->gadgets.add(nf);
                }
              }
            }
            break;
            case ATV_VALUE:
            case ATV_FINISHED:
              ok = true;
              break;
          }
      }
      if (!ok) {
        cout << "parse error in icon file" << endl;
        exit(1);
      }
    }
//    cout << "loaded " << algfig.size() << " algorithms" << endl;
  } else {
    cout << "failed to open vector icon files" << endl;
  }
}

/**
 * DX7 Envelope Tables are taken from legasynth-0.4.1.
 * Copyright (C) 2002 Juan Linietsky <coding@reduz.com.ar>
 * released under GNU GPL v2 (or later at your option)
 */

float EG_rate_rise_duration[128] = {
	38.00000 ,34.96000 ,31.92000 ,28.88000 ,25.84000 ,
	22.80000 ,20.64000 ,18.48000 ,16.32000 ,14.16000 ,
	12.00000 ,11.10000 ,10.20000 ,9.30000 ,8.40000 ,
	7.50000 ,6.96000 ,6.42000 ,5.88000 ,5.34000 ,
	4.80000 ,4.38000 ,3.96000 ,3.54000 ,3.12000 ,
	2.70000 ,2.52000 ,2.34000 ,2.16000 ,1.98000 ,
	1.80000 ,1.70000 ,1.60000 ,1.50000 ,1.40000 ,
	1.30000 ,1.22962 ,1.15925 ,1.08887 ,1.01850 ,
	0.94813 ,0.87775 ,0.80737 ,0.73700 ,0.69633 ,
	0.65567 ,0.61500 ,0.57833 ,0.54167 ,0.50500 ,
	0.47300 ,0.44100 ,0.40900 ,0.37967 ,0.35033 ,
	0.32100 ,0.28083 ,0.24067 ,0.20050 ,0.16033 ,
	0.12017 ,0.08000 ,0.07583 ,0.07167 ,0.06750 ,
	0.06333 ,0.05917 ,0.05500 ,0.04350 ,0.03200 ,
	0.02933 ,0.02667 ,0.02400 ,0.02200 ,0.02000 ,
	0.01800 ,0.01667 ,0.01533 ,0.01400 ,0.01300 ,
	0.01200 ,0.01100 ,0.01000 ,0.00900 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00767 ,0.00733 ,
	0.00700 ,0.00633 ,0.00567 ,0.00500 ,0.00433 ,
	0.00367 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300 ,0.00300 ,0.00300 ,
	0.00300 ,0.00300 ,0.00300
};

float EG_rate_decay_duration[128] = {
	318.00000 ,283.75000 ,249.50000 ,215.25000 ,181.00000 ,
	167.80000 ,154.60001 ,141.39999 ,128.20000 ,115.00000 ,
	104.60000 ,94.20000 ,83.80000 ,73.40000 ,63.00000 ,
	58.34000 ,53.68000 ,49.02000 ,44.36000 ,39.70000 ,
	35.76000 ,31.82000 ,27.88000 ,23.94000 ,20.00000 ,
	18.24000 ,16.48000 ,14.72000 ,12.96000 ,11.20000 ,
	10.36000 ,9.52000 ,8.68000 ,7.84000 ,7.00000 ,
	6.83250 ,6.66500 ,6.49750 ,6.33000 ,6.16250 ,
	5.99500 ,5.82750 ,5.66000 ,5.10000 ,4.54000 ,
	3.98000 ,3.64833 ,3.31667 ,2.98500 ,2.65333 ,
	2.32167 ,1.99000 ,1.77333 ,1.55667 ,1.34000 ,
	1.22333 ,1.10667 ,0.99000 ,0.89667 ,0.80333 ,
	0.71000 ,0.65000 ,0.59000 ,0.53000 ,0.47000 ,
	0.41000 ,0.32333 ,0.23667 ,0.15000 ,0.12700 ,
	0.10400 ,0.08100 ,0.07667 ,0.07233 ,0.06800 ,
	0.06100 ,0.05400 ,0.04700 ,0.04367 ,0.04033 ,
	0.03700 ,0.03300 ,0.02900 ,0.02500 ,0.02333 ,
	0.02167 ,0.02000 ,0.01767 ,0.01533 ,0.01300 ,
	0.01133 ,0.00967 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800 ,0.00800 ,0.00800 ,
	0.00800 ,0.00800 ,0.00800
};

float EG_rate_decay_percent[128] = {
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00501 ,0.01001 ,0.01500 ,
	0.02000 ,0.02800 ,0.03600 ,0.04400 ,0.05200 ,
	0.06000 ,0.06800 ,0.07600 ,0.08400 ,0.09200 ,
	0.10000 ,0.10800 ,0.11600 ,0.12400 ,0.13200 ,
	0.14000 ,0.15000 ,0.16000 ,0.17000 ,0.18000 ,
	0.19000 ,0.20000 ,0.21000 ,0.22000 ,0.23000 ,
	0.24000 ,0.25100 ,0.26200 ,0.27300 ,0.28400 ,
	0.29500 ,0.30600 ,0.31700 ,0.32800 ,0.33900 ,
	0.35000 ,0.36500 ,0.38000 ,0.39500 ,0.41000 ,
	0.42500 ,0.44000 ,0.45500 ,0.47000 ,0.48500 ,
	0.50000 ,0.52000 ,0.54000 ,0.56000 ,0.58000 ,
	0.60000 ,0.62000 ,0.64000 ,0.66000 ,0.68000 ,
	0.70000 ,0.73200 ,0.76400 ,0.79600 ,0.82800 ,
	0.86000 ,0.89500 ,0.93000 ,0.96500 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000
	
};

float EG_rate_rise_percent[128] = {
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00001 ,0.00001 ,0.00001 ,
	0.00001 ,0.00001 ,0.00501 ,0.01001 ,0.01500 ,
	0.02000 ,0.02800 ,0.03600 ,0.04400 ,0.05200 ,
	0.06000 ,0.06800 ,0.07600 ,0.08400 ,0.09200 ,
	0.10000 ,0.10800 ,0.11600 ,0.12400 ,0.13200 ,
	0.14000 ,0.15000 ,0.16000 ,0.17000 ,0.18000 ,
	0.19000 ,0.20000 ,0.21000 ,0.22000 ,0.23000 ,
	0.24000 ,0.25100 ,0.26200 ,0.27300 ,0.28400 ,
	0.29500 ,0.30600 ,0.31700 ,0.32800 ,0.33900 ,
	0.35000 ,0.36500 ,0.38000 ,0.39500 ,0.41000 ,
	0.42500 ,0.44000 ,0.45500 ,0.47000 ,0.48500 ,
	0.50000 ,0.52000 ,0.54000 ,0.56000 ,0.58000 ,
	0.60000 ,0.62000 ,0.64000 ,0.66000 ,0.68000 ,
	0.70000 ,0.73200 ,0.76400 ,0.79600 ,0.82800 ,
	0.86000 ,0.89500 ,0.93000 ,0.96500 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000 ,1.00000 ,1.00000 ,
	1.00000 ,1.00000 ,1.00000
};
