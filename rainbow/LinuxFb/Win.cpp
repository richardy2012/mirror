#include "LinuxFbLocal.h"

#define LLOG(x)       //LOG(x)

NAMESPACE_UPP

//video
int fbfd = -1;
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
long int screensize = 0;
char *fbp = 0;

//mouse
int mouse_fd = -1;
bool mouse_imps2 = false;
Point mousep;
dword mouseb = 0;

//keyb
int keyb_fd = -1;
int cvt = -1;

void FBQuitSession()
{
	Ctrl::EndSession();
}

int pend = 0;

bool FBIsWaitingEvent()
{
	pend = readevents(0);
	return pend > 0;
}

bool FBProcessEvent(bool *quit)
{
	if(pend)
	{
		if(pend & 1) handle_mouse();
		if(pend & 2) handle_keyboard();
		pend = 0; //need to reset, since with repeated call is not updated here, would stuck
		return true;
	}
	return false;
}

void FBSleep(int ms)
{
	pend = readevents(ms); //interruptable sleep
}

void FBUpdate(const Rect& inv)
{
	//FIXME accelerate
	const ImageBuffer& framebuffer = Ctrl::GetFrameBuffer();
	memcpy(fbp, ~framebuffer, framebuffer.GetLength() * sizeof(RGBA));
}

void FBFlush()
{

}

int oldvt;
struct termios oldtermios;
int oldmode;

int entervt()
{
	struct termios kt;

	if(cvt > 0) {
		struct vt_stat vtst;
		if(ioctl(keyb_fd, VT_GETSTATE, &vtst) == 0)
			oldvt = vtst.v_active;
		if(ioctl(keyb_fd, VT_ACTIVATE, cvt) == 0)
			ioctl(keyb_fd, VT_WAITACTIVE, cvt);
	}

	if(tcgetattr(keyb_fd, &oldtermios) < 0) {
		fprintf(stderr, "Error: saving terminal attributes");
		return -1;
	}

	if(ioctl(keyb_fd, KDGKBMODE, &oldmode) < 0) {
		fprintf(stderr, "Error: saving keyboard mode");
		return -1;
	}

	kt = oldtermios;
	kt.c_lflag &= ~(ICANON | ECHO | ISIG);
	kt.c_iflag &= ~(ISTRIP | IGNCR | ICRNL | INLCR | IXOFF | IXON);
	kt.c_cc[VMIN] = 0;
	kt.c_cc[VTIME] = 0;

	if(tcsetattr(keyb_fd, TCSAFLUSH, &kt) < 0) {
		fprintf(stderr, "Error: setting new terminal attributes");
		return -1;
	}

	if(ioctl(keyb_fd, KDSKBMODE, K_MEDIUMRAW) < 0) {
		fprintf(stderr, "Error: setting keyboard in mediumraw mode");
		return -1;
	}

	if(ioctl(keyb_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		fprintf(stderr, "Error: setting keyboard in graphics mode");
		return -1;
	}

	ioctl(keyb_fd, VT_LOCKSWITCH, 1);
	
	return 0;
}

void leavevt()
{
	if(oldmode < 0) return;

	if(ioctl(keyb_fd, KDSETMODE, KD_TEXT) < 0)
		fprintf(stderr, "Error: setting keyboard in text mode");
		
	if(ioctl(keyb_fd, KDSKBMODE, oldmode) < 0)
		fprintf(stderr, "Error: restoring keyboard mode");

	if(tcsetattr(keyb_fd, TCSAFLUSH, &oldtermios) < 0)
		fprintf(stderr, "Error: setting new terminal attributes");

	oldmode = -1;

	ioctl(keyb_fd, VT_UNLOCKSWITCH, 1);

	if(oldvt > 0)
		ioctl(keyb_fd, VT_ACTIVATE, oldvt);
}

void FBInit(const String& fbdevice)
{
	Ctrl::InitFB();
	Font::SetStdFont(ScreenSans(12)); //FIXME general handling

	fbfd = open(fbdevice, O_RDWR);
	if (!fbfd) {
		fprintf(stderr, "Error: cannot open framebuffer device.\n");
		exit(-1);
	}
	LLOG("The framebuffer device was opened successfully.\n");

	if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {
		fprintf(stderr, "Error reading fixed information.\n");
		exit(-2);
	}

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
		fprintf(stderr, "Error reading variable information.\n");
		exit(-3);
	}
	RLOG("Framebuffer opened: " << fbdevice << ": " << vinfo.xres << "x" << vinfo.yres << " @ " << vinfo.bits_per_pixel);

	screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8; //bytes

	fbp = (char*)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if((intptr_t)fbp == -1) {
		fprintf(stderr, "Error: failed to map framebuffer device to memory.\n");
		exit(-4);
	}
	LLOG("The framebuffer device was mapped to memory successfully.\n");

	Size fbsz(vinfo.xres, vinfo.yres);
	Ctrl::SetFramebufferSize(fbsz);

	//mouse

	//mousep = fbsz / 2;
	mousep.Clear();	

	static const char *mice[] = {
		"/dev/input/mice"
		, "/dev/usbmouse"
		, "/dev/psaux"
		, NULL
	};

	for(int i=0; (mouse_fd < 0) && mice[i]; ++i) {
		mouse_fd = open(mice[i], O_RDWR, 0);
		if(mouse_fd < 0) mouse_fd = open(mice[i], O_RDONLY, 0);
		if(mouse_fd >= 0) {
			set_imps2(mouse_fd, 1);
			if(mouse_imps2 = has_imps2(mouse_fd)) {
				LLOG("IMPS2 mouse enabled: " << mice[i]);
			}
			else
				RLOG("no IMPS2 mouse present");
		}
		else
			fprintf(stderr, "Error: failed to open %s.\n", mice[i]);
	}

	//keyb

	static const char* const tty0[] = {
		"/dev/tty0"
		, "/dev/vc/0"
		, NULL
	};
	static const char* const vcs[] = {
		"/dev/vc/"
		, "/dev/tty"
		, NULL
	};

	int tfd = -1;
	for(int i=0; tty0[i] && (tfd < 0); ++i)
		tfd = open(tty0[i], O_WRONLY, 0);
	if(tfd < 0)
		tfd = dup(0);
	ASSERT(tfd>=0);

	ioctl(tfd, VT_OPENQRY, &cvt);
	close(tfd);
	LLOG("probable new VT: " << cvt);

	if(geteuid() != 0)
	{
		fprintf(stderr, "Error: not running as ROOT, mouse handling pobably unavailable\n");
	}
	else if(cvt > 0) {
		LLOG("try to open the NEW assigned VT: " << cvt);
		for(int i=0; vcs[i] && (keyb_fd < 0); ++i) {
			char path[32];
			snprintf(path, 32, "%s%d", vcs[i], cvt);
			keyb_fd = open(path, O_RDWR, 0);

			if(keyb_fd < 0)
				continue;	
			
			LLOG("TTY path opened: " << path);
			tfd = open("/dev/tty", O_RDWR, 0);
			if(tfd >= 0) {
				LLOG("detaching from local stdin/out/err");
				ioctl(tfd, TIOCNOTTY, 0);
				close(tfd);
			}
			else
				fprintf(stderr, "Error: detaching from local stdin/out/err\n");
		}
	}

	if(keyb_fd < 0) {
		LLOG("Using already assigned VT, must not detach");
		struct vt_stat vtstate;

		keyb_fd = open("/dev/tty", O_RDWR);

		if(ioctl(keyb_fd, VT_GETSTATE, &vtstate) < 0) {
			cvt = 0;
		} else {
			cvt = vtstate.v_active;
		}
	}

	ASSERT(keyb_fd>=0);
	oldmode = -1;

	{
		int d;
		if(ioctl(keyb_fd, KDGKBMODE, &d) < 0) {
			close(keyb_fd);
			keyb_fd = -1;
			fprintf(stderr, "Error: opening a console terminal");
			exit(5);
		}
	}

	LLOG("tty opened: " << keyb_fd);

	dupkmap(keyb_fd);

	entervt();
	
	pend = 4; //fake video expose event to cause first paint
}

void FBDeInit()
{
	Ctrl::ExitFB();
	munmap(fbp, screensize);
	close(fbfd);
	
	leavevt();
	close(mouse_fd);
}

END_UPP_NAMESPACE
