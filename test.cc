#if 0
while(true) {  
  char b[3];
  b[0] = 153;
  b[1] = 36;
  b[2] = 127;
  sleep(1);
  write(midi.getFD(), b, 3);
}  
#endif
  
#if 0
  // enumerate devices (not really related to purpose of the echo program
  // but shows how to get information about devices)
  int i, n;
  CFStringRef pname, pmanuf, pmodel;  
  char name[64], manuf[64], model[64];
  
  n = MIDIGetNumberOfDevices();
  for (i = 0; i < n; ++i) {
    MIDIDeviceRef dev = MIDIGetDevice(i);
    
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
  }
  
  MIDIClientRef client = 0;
  MIDIClientCreate(CFSTR("DX7 Client"),
                   0,
                   0,
                   &client);
  
  MIDIPortRef inPort = 0, outPort = 0;
  MIDIInputPortCreate(client,
                      CFSTR("DX7 In"),
                      myread,
                      0,
                      &inPort);
  MIDIOutputPortCreate(client,
                      CFSTR("DX7 Out"),
                      &outPort);
  
  MIDIEndpointRef src = MIDIGetSource(1);
  MIDIPortConnectSource(inPort, src, NULL);

  MIDIPacketList list;
  list.numPackets = 1;
  list.packet[0].timeStamp = 0;
  list.packet[0].length = 3;
  list.packet[0].data[0] = 153;
  list.packet[0].data[1] = 36;
  list.packet[0].data[2] = 127;
  
  MIDIEndpointRef dst = MIDIGetDestination(1);
  MIDISend(outPort, dst, &list);
#endif
