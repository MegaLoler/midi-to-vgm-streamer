#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#define CHUNK_TYPE_HEADER 0x6468544D
#define CHUNK_TYPE_TRACK 0x6b72544d

#define AY_CLOCK 1789773

// polyphony mode
// ring mode doesnt care about midi channel, and allocates whichever ay channel available
// voice mode allocates each midi channel to each ay channel
#define MODE_RING 0
#define MODE_VOICE 1
#define MODE MODE_RING

#define DEBUG
//#define VERBOSE
//#define REAL_VERBOSE

// which midi notes are currently playing on ay channels
int ayNotes[6] = { -1, -1, -1, -1, -1, -1 };
// which midi channels are currently assigned to ay channels
int ayMidiChannels[6] = { 0, 0, 0, 0, 0, 0 };
// play orders, for choosing which to steal when all are taken
uint8_t ayNoteOrders[6] = {0, 0, 0, 0, 0, 0};
uint8_t noteIndex = 0;
// keep track of how 'bended' each midi channel is at any time
// in semitones, -2 to +2
double pitchBends[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// processing state
int format; // the midi file format
int chunks; // remaining chunks
int division; // midi division spec
int tempo = 120; // tempo, assume 120 for now ? ? ?

// get pitch frequency for a midi note
// equal temperatment, a440
// account for pitch bend percentage
double midi2freq(int note, double bend)
{
	return 440 * pow(2, ((note + bend) - 69) / 12.0);
}

// get ay period value for a pitch
double freq2period(double freq)
{
	return 111861 / freq;
}

// print out raw bytes
// (little endian)
void printBytes(uint64_t *data, int bytes)
{
	int i;
	for(i = 0; i < bytes; i++)
	{
		uint8_t byte = *((uint8_t *)data + i);
		putchar(byte);
	}
}

// print 64bit value
void print64(uint64_t value)
{
	uint64_t data = value;
	printBytes(&data, sizeof(uint64_t));
}

// print 32bit value
void print32(uint32_t value)
{
	uint64_t data = value;
	printBytes(&data, sizeof(uint32_t));
}

// print 16bit value
void print16(uint16_t value)
{
	uint64_t data = value;
	printBytes(&data, sizeof(uint16_t));
}

// print 8bit value
void print8(uint8_t value)
{
	uint64_t data = value;
	printBytes(&data, sizeof(uint8_t));
}

// print commands to write to an ay
void printAyCommand(uint8_t address, uint8_t data, int ay)
{
	address &= 0x0F;
	if(ay == 1) address += 0x80;
	print8(0xA0);
	print8(address);
	print8(data);
}

// print out commands for waiting for n samples
void printWaitSamples(int n)
{
	if(n == 0) return;
	else if(n == 735) print8(0x62);
	else if(n == 882) print8(0x63);
	else if(n > 0 && n <= 16) print8(0x70 + (n - 1));
	else if(n > 0xFFFF)
	{
		print8(0x61);
		print16(0xFFFF);
		printWaitSamples(n - 0xFFFF);
	}
	else
	{
		print8(0x61);
		print16(n);
	}
}

// print a vgm header
void printVGMHeader()
{
	// print the file identification
	printf("Vgm ");
	// print eof offset (we don't know since we are generating this real time)
	print32(0);
	// print version number
	// in bcd or somethin
	print8(0x51);
	print8(0x01);
	print8(0x00);
	print8(0x00);
	// print a bunch of clocks we dont care about
	print32(0);
	print32(0);
	print32(0);
	// we dont know about samples yet
	print32(0);
	// no loop for now
	print32(0);
	print32(0);
	// no rate scaling
	print32(0);
	// more chip values and clocks we dont care about
	print16(0);
	print8(0);
	print8(0);
	print32(0);
	print32(0);
	// we'll end the header at 0x80
	print32(0x4C);
	// more clocks n stuff no1 cares about here (wow im mean sorry)
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	print32(0);
	// our chips! ay clock rate
	print32(AY_CLOCK | 0x40000000);
	// normal ay9810
	print8(0);
	// single output ? ? i dont actually kno
	print8(1);
	// dont care
	print8(0);
	print8(0);
	print8(0);
	print8(0);
	print8(0);
	print8(0);
	// end of header!
}

// get an ay channel with a given note order
int getAyChannelWithNoteOrder(uint8_t noteOrder)
{
	int i;
	for(i = 0; i < 6; i++)
	{
		// this is it!
		if(ayNoteOrders[i] == noteOrder) return i;
	}

	// not found
	return -1;
}

// get the first available ay channel not playing anything
int getFreeAyChannel()
{
	int i;
	for(i = 0; i < 6; i++)
	{
		// this one is free!
		if(ayNotes[i] == -1) return i;
	}

	// there were none!
	// so time to steal one
	uint8_t j;
	for(j = noteIndex + 1; j != noteIndex; j++)
	{
		// see if we can steal one thats this old
		int channel = getAyChannelWithNoteOrder(j);
		if(channel != -1) return channel;
	}

	// well none were found still what
	return -1;
}

// find the ay channel playing the note on a midi channel
int getAyChannelPlayingNote(int note, int channel)
{
	int i;
	for(i = 0; i < 6; i++)
	{
		// this is the one!
		// actualy ignore the channel????? note offs on other channels???? weird
		//if(ayNotes[i] == note && ayMidiChannels[i] == channel) return i;
		if(ayNotes[i] == note) return i;
	}

	// there were none!
	return -1;
}

// set the volume of one of the 6 ay channels
void setAyVolume(int channel, uint8_t volume)
{
	printAyCommand(8 + channel % 3, volume, channel / 3);
}

// set the freq of one of the 6 ay channels
void setAyFreq(int channel, double freq)
{
	double period = freq2period(freq);
	printAyCommand((channel % 3) * 2, (int)period % 0x100, channel / 3);
	printAyCommand(1 + (channel % 3) * 2, (int)(period / 0x100), channel / 3);
}

// update all the pitches on a midi channel
void updatePitches(int channel)
{
	// go through each ay channel
	int i;
	for(i = 0; i < 6; i++)
	{
		// if this channel is playing on this midi channel
		// then we've come to the right place
		if(ayMidiChannels[i] == channel)
		{
			// get the new freq and set it
			double freq = midi2freq(ayNotes[i], pitchBends[channel]);
			setAyFreq(i, freq);
		}
	}
}

// play ay note
void ayPlayNote(int channel, int note, int velocity)
{
	// which ay channel does this go to?
	int ayChannel;
	if(MODE == MODE_RING)
	{
		// first free available channel
		ayChannel = getFreeAyChannel();
	}
	else if(MODE == MODE_VOICE)
	{
		// midi channel indicates
		ayChannel = channel;
	}

	// values for the ay
	uint8_t volume = (velocity & 0x7F) >> 3;
	double freq = midi2freq(note, pitchBends[channel]);

	// watch out for invalid channels
	if(ayChannel < 0 || ayChannel >= 6) return;

	// send messages to ay
	setAyVolume(ayChannel, volume);
	setAyFreq(ayChannel, freq);

	// set this channel as taken
	ayNotes[ayChannel] = note;
	// set this ay channel to this midi channel
	ayMidiChannels[ayChannel] = channel;
	// set the note order
	ayNoteOrders[ayChannel] = noteIndex++;
}

// stop an ay note
void ayStopNote(int channel, int note, int velocity)
{
	// which ay channel does this go to?
	int ayChannel;
	if(MODE == MODE_RING)
	{
		// get which ay channel this note is playing on
		ayChannel = getAyChannelPlayingNote(note, channel);
	}
	else if(MODE == MODE_VOICE)
	{
		// midi channel indicates
		ayChannel = channel;
	}

	// watch out for invalid channels
	if(ayChannel < 0 || ayChannel >= 6) return;

	// note off, i say!
	setAyVolume(ayChannel, 0);

	// set this channel as free
	ayNotes[ayChannel] = -1;
}

// get the name of a midi note value
char *getMidiNoteName(int note)
{
	int chromaticId = note % 12;
	int octave = note / 12 - 1;
	char letter;
	int sharp = 0;
	switch(chromaticId)
	{
		case 1:
			sharp = 1;
		case 0:
			letter = 'C';
			break;
		case 3:
			sharp = 1;
		case 2:
			letter = 'D';
			break;
		case 4:
			letter = 'E';
			break;
		case 6:
			sharp = 1;
		case 5:
			letter = 'F';
			break;
		case 8:
			sharp = 1;
		case 7:
			letter = 'G';
			break;
		case 10:
			sharp = 1;
		case 9:
			letter = 'A';
			break;
		case 11:
			letter = 'B';
			break;
	}

	char *name = (char*)malloc(32);
	if(sharp) sprintf(name, "%c#%i", letter, octave);
	else sprintf(name, "%c%i", letter, octave);
	return name;
}

// handle midi events!
// a note was pressed
void onNoteOn(int channel, int note, int velocity)
{
#ifdef DEBUG
	fprintf(stderr, "NOTE_ON(channel=%i, note=%s, velocity=%i)\n", channel, getMidiNoteName(note), velocity);
#endif

	// send to ay
	ayPlayNote(channel, note, velocity);
}

// a note was released
void onNoteOff(int channel, int note, int velocity)
{
#ifdef DEBUG
	fprintf(stderr, "NOTE_OFF(channel=%i, note=%s, velocity=%i)\n", channel, getMidiNoteName(note), velocity);
#endif

	// send to ay
	ayStopNote(channel, note, velocity);
}

// handle delta times
void onDeltaTime(int delta)
{
#ifdef DEBUG
#ifdef VERBOSE
	fprintf(stderr, "Delta: %i\n", delta);
#endif
#endif

	// print out the sample time to wait
	int samples;
	if(division & 0x8000)
	{
		int ticksPerBeat = division & 0x7FFF;
		double seconds = tempo * delta / ticksPerBeat / 1000;
		samples = (int)(44100 * seconds);
	}
	else
	{
		uint8_t smpte = (division & 0x7F00) >> 8;
		double framesPerSecond;
		if(smpte == 0) framesPerSecond = 24;
		else if(smpte == 1) framesPerSecond = 25;
		else if(smpte == 2) framesPerSecond = 29.97;
		else if(smpte == 3) framesPerSecond = 30;
		uint8_t ticksPerFrame = division & 0x00FF;
		double seconds = tempo * delta / ticksPerFrame / 1000;
		samples = (int)(44100 * seconds);
	}
	samples = delta * 30;
	printWaitSamples(samples);
}

// time 4 headers y'all
void onHeader()
{
	// output a vgm header
	printVGMHeader();

	// init ays
	// enable a b and c
	// on both ays
	printAyCommand(0x07, 0x38, 0);
	printAyCommand(0x07, 0x38, 1);
}

// read bytes big endian
uint8_t *readBytesBig(int bytes)
{
	uint8_t *ptr = (uint8_t *)malloc(bytes);
	int i;
	for(i = bytes - 1; i >= 0; i--)
	{
		(*(ptr + i)) = getchar();
	}
	return ptr;
}

// read bytes little endian
uint8_t *readBytes(int bytes)
{
	uint8_t *ptr = (uint8_t *)malloc(bytes);
	int i;
	for(i = 0; i < bytes; i++)
	{
		(*(ptr + i)) = getchar();
	}
	return ptr;
}

// read in a midi variable length quantity
// deduct bytes read from bytes
uint32_t readVariableLengthQuantity(int *bytes)
{
	uint32_t value = 0;
	uint8_t byte;
	do
	{
		value <<= 7;
		byte = getchar();
		value += byte & 0x7F;
		(*bytes)--;
	}
	while(byte & 0x80);
	return value;
}

// process a header chunk
void processHeaderChunk(int bytes)
{
	// get the midi file format
	uint16_t formatValue = *(uint16_t *)(readBytesBig(sizeof(uint16_t)));
	bytes -= sizeof(uint16_t);

	// get the number of tracks
	uint16_t tracksValue = *(uint16_t *)(readBytesBig(sizeof(uint16_t)));
	bytes -= sizeof(uint16_t);

	// get the division specification
	uint16_t divisionValue = *(uint16_t *)(readBytesBig(sizeof(uint16_t)));
	bytes -= sizeof(uint16_t);

	// read out any remaining bytes (that shouldn't be here but yeah)
	readBytes(bytes);

#ifdef DEBUG
#ifdef VERBOSE
	fprintf(stderr, "HEADER\n======\n");
	fprintf(stderr, "\tFormat: 0x%x\n", formatValue);
	fprintf(stderr, "\tTracks: %i\n", tracksValue);
	fprintf(stderr, "\tDivision: 0x%x\n", divisionValue);
#endif
#endif

	// store the information
	format = formatValue;
	chunks = tracksValue;
	division = divisionValue;

	// we need a type 0 midi file tho, since we want to process realtime input
	if(format != 0)
	{
		fprintf(stderr, "WARNING! Uh, I can only work properly with type 0 midi files, so this might not be what you expect!!\n");
	}

	// do things to do after reading a header
	onHeader();
}

// process a meta event message
// deduct from bytes how many bytes were read
void processMetaMessage(int *bytes)
{
	// get the meta event type and how long it is
	uint8_t type = *(uint8_t *)readBytes(sizeof(uint8_t));
	(*bytes)--;
	uint32_t length = readVariableLengthQuantity(bytes);

	// set tempo
	if(type == 0x51)
	{
		// honestly what the fricky i dont understand any of this
		/*
		uint32_t tempoValue = *(uint32_t *)readBytes(length) & 0xFFFFFF;
		tempo = 60000000.0 / tempoValue;
		fprintf(stderr, "val: %i\n", tempoValue);
		fprintf(stderr, "temp set: %i\n", tempo);
		exit(0);
		*/
		readBytes(length);
		(*bytes) -= length;
	}
	// any other message
	else
	{
		// for now just read out the length
		readBytes(length);
		(*bytes) -= length;
	}
}

// process a sysex message
// deduct from bytes how many bytes were read
void processSystemMessage(int *bytes)
{
	// get how long the message is
	uint32_t length = readVariableLengthQuantity(bytes);

	// for now just read out the length
	readBytes(length);
	(*bytes) -= length;
}

// last event type, for running status
int lastType = 0;

// process a midi event message
// deduct from bytes how many bytes were read
void processMidiMessage(int delta, int type, int *bytes)
{
	// running status?
	int eventType;
	int channel;
	uint8_t arg1;
	uint8_t arg2;
	if(type & 0x80)
	{
		// get the new status byte and save it in case of running
		eventType = (type & 0xF0) >> 4;
		lastType = eventType;

		// get the information abotu the midi message
		channel = type & 0x0F;
		arg1 = *(uint8_t *)readBytes(sizeof(uint8_t));
		(*bytes)--;

		// these two commands only have 1 arg instead of 2
		if(eventType != 0x0C && eventType != 0x0D)
		{
			arg2 = *(uint8_t *)readBytes(sizeof(uint8_t));
			(*bytes)--;
		}
	}
	else
	{
		// restore running status byte
		eventType = lastType;

		// get the information abotu the midi message
		channel = type & 0x0F;
		// so in this case that 'type' was actually
		// the arg 1!
		arg1 = (uint8_t)type;

		// these two commands only have 1 arg instead of 2
		if(eventType != 0x0C && eventType != 0x0D)
		{
			arg2 = *(uint8_t *)readBytes(sizeof(uint8_t));
			(*bytes)--;
		}
	}

#ifdef DEBUG
#ifdef VERBOSE
	fprintf(stderr, "MIDI EVENT\n==========\n");
	fprintf(stderr, "\tType: 0x%x\n", eventType);
	fprintf(stderr, "\tChannel: 0x%x\n", channel);
	fprintf(stderr, "\tArgument 1: 0x%x\n", arg1);
	fprintf(stderr, "\tArgument 2: 0x%x\n", arg2);
#endif
#endif

	// handle delta times
	onDeltaTime(delta);

	// note off
	if(eventType == 0x08)
	{
		onNoteOff(channel, arg1, arg2);
	}
	// note on
	else if(eventType == 0x09)
	{
		if(arg2) onNoteOn(channel, arg1, arg2);
		else onNoteOff(channel, arg1, arg2);
	}
	// note aftertouch
	else if(eventType == 0x0A)
	{
	}
	// controller
	else if(eventType == 0x0B)
	{
	}
	// program change
	else if(eventType == 0x0C)
	{
	}
	// channel aftertouch
	else if(eventType == 0x0D)
	{
	}
	// pitch bend
	else if(eventType == 0x0E)
	{
		int arg = (arg1 & 0x7F) + ((arg2 & 0x7F) << 7);
		int pivot = arg - 0x2000;
		double percent = pivot / (double)0x1000;
		pitchBends[channel] = percent;
		updatePitches(channel);
	}
}

// process a midi message
// take care of counting down how many bytes are read
void processMessage(int *bytes)
{
	uint32_t deltaTime = readVariableLengthQuantity(bytes);
	uint8_t eventType = *(uint8_t *)readBytes(sizeof(uint8_t));
	(*bytes)--;

#ifdef DEBUG
#ifdef VERBOSE
#ifdef REAL_VERBOSE
	fprintf(stderr, "MESSAGE\n=======\n");
	fprintf(stderr, "\tDelta Time: 0x%x\n", deltaTime);
	fprintf(stderr, "\tEvent Type: 0x%x\n", eventType);
#endif
#endif
#endif

	// meta message
	if(eventType == 0xFF)
	{
		processMetaMessage(bytes);
		// clear running status
		lastType = 0;
	}
	// sysex message
	else if(eventType == 0xF0 || eventType == 0xF7)
	{
		processSystemMessage(bytes);
		// clear running status
		lastType = 0;
	}
	// midi event message
	else
	{
		processMidiMessage(deltaTime, eventType, bytes);
	}
}

// process a track chunk
void processTrackChunk(int bytes)
{
	// while there remain things to be read
	// process midi messages!
	while(bytes > 0)
	{
		processMessage(&bytes);
	}
	// read out any remaining bytes
	readBytes(bytes);
}

// process the body of a midi chunk
void processChunkBody(uint32_t chunkType, int bytes)
{
	// figure out which kind it is
	if(chunkType == CHUNK_TYPE_HEADER) processHeaderChunk(bytes);
	else if (chunkType == CHUNK_TYPE_TRACK) processTrackChunk(bytes);
}

// process a midi file chunk
void processChunk()
{
	uint32_t chunkType = *(uint32_t *)readBytes(sizeof(uint32_t));
	uint32_t chunkLength = *(uint32_t *)readBytesBig(sizeof(uint32_t));

#ifdef DEBUG
#ifdef VERBOSE
#ifdef REAL_VERBOSE
	fprintf(stderr, "CHUNK\n=====\n");
	fprintf(stderr, "\tType: 0x%x\n", chunkType);
	fprintf(stderr, "\tLength: 0x%x\n", chunkLength);
#endif
#endif
#endif

	processChunkBody(chunkType, chunkLength);
}

// process a midi file
void processMidi()
{
	// its just a collection of chunks
	// assume it has at least one chunk to begin with (the header chunk)
	chunks = 1;
	while(chunks--)
	{
		processChunk();
	}
}

// start
int main()
{
	// process the midi file
	processMidi();
	return 0;
}
