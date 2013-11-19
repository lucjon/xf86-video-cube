/*
 * XFree86 driver for Gamecube/Wii framebuffer(tm).
 *
 * This driver and its documentation are based on XFree86 Glide driver
 * made by Henrik Harmsen (hch@cd.chalmers.se or Henrik.Harmsen@erv.ericsson.se).
 *
 * The Gamecube/Wii kernel videocard driver lets us draw 2D images directly
 * to the framebuffer, using the special device /dev/fb0. We can use the XFree86
 * driver for framebuffer, but it draws in RGB mode, and the framebuffer only
 * understands images un YUV2 format. So we need to parse that image and translate
 * from RGB to YUV2 before drawing in the framebuffer. This driver does basically
 * that job, using a virtual framebuffer (the Shadow FrameBuffer) where the Xserver
 * draws in RGB, and in certain moments we update the framebuffer with the image
 * in YUV2 format. This driver does not provide hardware acceleration, and only
 * supports 640x480-16bpp, but for now it is usable.
 *
 * Author: 
 *	Marcos Novalbos (nuvalo@gmail.com)
 *	Based on the work from Henrik Harmsen (hch@cd.chalmers.se or Henrik.Harmsen@erv.ericsson.se)
 *
 * HISTORY
 *	2008-17-7 (Nuvalo)
 *	- First release, version 1.0
 *	- Bugs found:
 *	- Some parts of the screen aren't updated until some interaction with them
 *	- Take in count more compatible resolutions, like PAL
 *
 *	2010-24-12 (lucjon)
 *	- Import from CVS to Git/Github
 *	- Removed symbol lists
 *
 *	2013-XX-11 (DeltaResero)
 *	- Import missed files from CVS
 *	- Minor code optimizations and updates
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
//#include <asm/page.h>
#include <linux/fb.h>

#include "colormapst.h"
#include "xf86.h"
#include "xf86_OSproc.h"
#include "mipointer.h"
#include "micmap.h"

#include "globals.h"

#ifdef HAVE_XEXTPROTO_71
#include <X11/extensions/dpmsconst.h>
#else
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#endif

#include "fb.h"
#include "xf86cmap.h"
#include "shadowfb.h"

#include "compat-api.h"

#define TRUE 1
#define FALSE 0

typedef signed char s8;
typedef unsigned char u8;
typedef signed short int s16;
typedef unsigned short int u16;
typedef signed long int s32;
typedef unsigned long int u32;

/* Card-specific driver information */

#define CUBEPTR(p) ((CUBEPtr)((p)->driverPrivate))

typedef struct {
	u8* ShadowPtr;
	u32 ShadowPitch;
	u32 SST_Index;
	CloseScreenProcPtr  CloseScreen;
	Bool Blanked;
	Bool OnAtExit;
	Bool CubeInitiated;
	EntityInfoPtr pEnt;
	OptionInfoPtr Options;
	s32 mapped_offset;
	s32 mapped_memlen;
	u8* mapped_mem;
	int console_fd;
} CUBERec, *CUBEPtr;

static const OptionInfoRec * CUBEAvailableOptions(int chipid, int busid);
static void CUBEIdentify(int flags);
static Bool CUBEProbe(DriverPtr drv, int flags);
static Bool CUBEPreInit(ScrnInfoPtr pScrn, int flags);
static Bool CUBEScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool CUBEEnterVT(VT_FUNC_ARGS_DECL);
static void CUBELeaveVT(VT_FUNC_ARGS_DECL);
static Bool CUBECloseScreen(CLOSE_SCREEN_ARGS_DECL);
static Bool CUBESaveScreen(ScreenPtr pScreen, int mode);
static void CUBEFreeScreen(FREE_SCREEN_ARGS_DECL);
static void CUBERefreshArea(ScrnInfoPtr pScrn, int num, BoxPtr pbox);
static Bool CUBEModeInit(ScrnInfoPtr pScrn, DisplayModePtr mode);
static void CUBERestore(ScrnInfoPtr pScrn, Bool Closing);
static void CUBERefreshAll(ScrnInfoPtr pScrn);

static void	CUBEDisplayPowerManagementSet(ScrnInfoPtr pScrn,
									int PowerManagementMode,
									int flags);


static int
initFrameBuffer(ScrnInfoPtr pScrn);
#define CUBE_VERSION 1.6-0
#define CUBE_NAME "CUBE"
#define CUBE_DRIVER_NAME "cube"
#define CUBE_MAJOR_VERSION 1
#define CUBE_MINOR_VERSION 6
#define CUBE_PATCHLEVEL 0

/*
 * This contains the functions needed by the server after loading the
 * driver module.  It must be supplied, and gets added the driver list by
 * the Module Setup function in the dynamic case.  In the static case a
 * reference to this is compiled in, and this requires that the name of
 * this DriverRec be an upper-case version of the driver name.
 */

_X_EXPORT DriverRec CUBE = {
	CUBE_VERSION,
	CUBE_DRIVER_NAME,
	CUBEIdentify,
	CUBEProbe,
	CUBEAvailableOptions,
	NULL,
	0
};

typedef enum {
	OPTION_ON_AT_EXIT,
	OPTION_CUBEDEVICE
} CUBEOpts;

static const OptionInfoRec CUBEOptions[] = {
	{ OPTION_ON_AT_EXIT, "OnAtExit", OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_CUBEDEVICE, "CubeDevice", OPTV_INTEGER, {0}, FALSE },
	{ -1, NULL, OPTV_NONE, {0}, FALSE }
};

/* Supported chipsets */
static SymTabRec CUBEChipsets[] = {
	{ 0, "Hollywood" },
	{-1, NULL }
};


/*
 * This is the algorithm borrowed from libSDL port for gc-linux,
 * made by Albert "isobel" Herranz
 *
 * www.gc-linux.org
 *
 */
#define RGB2YUV_SHIFT 16
#define RGB2YUV_LUMA 16
#define RGB2YUV_CHROMA 128

#define Yr ((int)( 0.299*(1<<RGB2YUV_SHIFT)))
#define Yg ((int)( 0.587*(1<<RGB2YUV_SHIFT)))
#define Yb ((int)( 0.114*(1<<RGB2YUV_SHIFT)))

#define Ur ((int)(-0.169*(1<<RGB2YUV_SHIFT)))
#define Ug ((int)(-0.331*(1<<RGB2YUV_SHIFT)))
#define Ub ((int)( 0.500*(1<<RGB2YUV_SHIFT)))

#define Vr ((int)( 0.500*(1<<RGB2YUV_SHIFT)))	/* same as Ub */
#define Vg ((int)(-0.419*(1<<RGB2YUV_SHIFT)))
#define Vb ((int)(-0.081*(1<<RGB2YUV_SHIFT)))

#define clamp(x, y, z) ((z < x) ? x : ((z > y) ? y : z))

static u32 r_Yr[256];
static u32 g_Yg_[256];
static u32 b_Yb[256];
static u32 r_Ur[256];
static u32 g_Ug_[256];
static u32 b_Ub[256];
/* static u32 r_Vr[256]; // space and cache optimisation */
#define r_Vr b_Ub
static u32 g_Vg_[256];
static u32 b_Vb[256];

static u8 RGB16toY[1 << 16];
static u8 RGB16toU[1 << 16];
static u8 RGB16toV[1 << 16];

static void initRGB2YUVTables(void)
{
	int i;
	int r, g, b;

	for (i = 0; i < 256; i++) {
		r_Yr[i] = Yr * i;
		g_Yg_[i] = Yg * i + (RGB2YUV_LUMA << RGB2YUV_SHIFT);
		b_Yb[i] = Yb * i;
		r_Ur[i] = Ur * i;
		g_Ug_[i] = Ug * i + (RGB2YUV_CHROMA << RGB2YUV_SHIFT);
		b_Ub[i] = Ub * i;
		r_Vr[i] = Vr * i;
		g_Vg_[i] = Vg * i + (RGB2YUV_CHROMA << RGB2YUV_SHIFT);
		b_Vb[i] = Vb * i;
	}

	for (i = 0; i < 1 << 16; i++) {
#if 0
		/* RGB555, extraction and (incorrect) scaling */
		r = ((i >> 8) & 0xf8);
		g = ((i >> 3) & 0xf8);
		b = ((i << 2) & 0xf8);
#endif
#if 0
		/* RGB565, extraction and (incorrect) scaling */
		r = ((i >> 8) & 0xf8); /* ((i>>11)&0x1f)<<3*/
		g = ((i >> 3) & 0xfc); /* ((i>>5)&0x3f)<<2*/
		b = ((i << 3) & 0xf8); /* ((i>>0)&0x1f)<<3*/
#endif
		/* RGB565 */
		r = ((i >> 11) & 0x1f);
		g = ((i >> 5) & 0x3f);
		b = ((i >> 0) & 0x1f);

		/* fast (approximated) scaling to 8 bits, thanks to Masken */
		r = (r << 3) | (r >> 2);
		g = (g << 2) | (g >> 4);
		b = (b << 3) | (b >> 2);
#if 0
		/* scaling to 8 bits */
		r = (r * 0xff) / 0x1f;
		g = (g * 0xff) / 0x3f;
		b = (b * 0xff) / 0x1f;
#endif
		RGB16toY[i] =
			clamp(16, 235,
				(r_Yr[r] + g_Yg_[g] + b_Yb[b]) >> RGB2YUV_SHIFT);
		RGB16toU[i] =
			clamp(16, 240,
				(r_Ur[r] + g_Ug_[g] + b_Ub[b]) >> RGB2YUV_SHIFT);
		RGB16toV[i] =
			clamp(16, 240,
				(r_Vr[r] + g_Vg_[g] + b_Vb[b]) >> RGB2YUV_SHIFT);
	}
}


static inline u32 rgbrgb16toyuy2(u16 rgb1, u16 rgb2)
{
	register int Y1, Cb, Y2, Cr;
	u16 rgb;

	/* fast path, thanks to bohdy */
	if (!(rgb1 | rgb2)) {
		return 0x00800080;	/* black, black */
	}

	if (rgb1 == rgb2) {
		/* fast path, thanks to isobel */
		Y1 = Y2 = RGB16toY[rgb1];
		Cb = RGB16toU[rgb1];
		Cr = RGB16toV[rgb1];
	} else {
		Y1 = RGB16toY[rgb1];
		Y2 = RGB16toY[rgb2];

		/*
		 * This computes a new rgb using the mean of r,g,b between
		 * rgb1 and rgb2. Tricky.
		 * --isobel
		 */
#if 0
		/* this is for RGB555 */
		rgb = (((rgb1 >> 1) & ~0x8421) + ((rgb2 >> 1) & ~0x8421))
			+ ((rgb1 & rgb2) & 0x0842);
#endif

		/* this is for RGB565 */
		rgb = (((rgb1 >> 1) & ~0x8410) + ((rgb2 >> 1) & ~0x8410))
			+ ((rgb1 & rgb2) & 0x0841);

		Cb = RGB16toU[rgb];
		Cr = RGB16toV[rgb];
	}

	return (((char)Y1) << 24) | (((char)Cb) << 16) | (((char)Y2) << 8)
			| (((char)Cr) << 0);
}


/*
*
* This part was based on the Xfree86 Glide driver, and some parts were
* taken from the fbdev driver
*
*/

#ifdef XFree86LOADER

static MODULESETUPPROTO(cubeSetup);

static XF86ModuleVersionInfo cubeVersRec =
{
	"cube",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	CUBE_MAJOR_VERSION, CUBE_MINOR_VERSION, CUBE_PATCHLEVEL,
	ABI_CLASS_VIDEODRV,			/* This is a video driver */
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{0,0,0,0}
};

_X_EXPORT XF86ModuleData cubeModuleData = { &cubeVersRec, cubeSetup, NULL };

static pointer
cubeSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;
	int errmaj2 = 0, errmin2 = 0;

	if (!setupDone)
	{

		setupDone = TRUE;
		/* This module should be loaded only once */
		*errmaj = LDR_ONCEONLY;
		xf86AddDriver(&CUBE, module, 0);

		/*
		 * The return value must be non-NULL on success even though there
		 * is no TearDownProc.
		 */
	return (pointer)1;
	}
  else
	{
	if (errmaj)
		*errmaj = LDR_ONCEONLY;
	return NULL;
	}
}
#endif /* XFree86LOADER */

static Bool
CUBEGetRec(ScrnInfoPtr pScrn)
{
	/*
	 * Allocate an CUBERec, and hook it into pScrn->driverPrivate.
	 * pScrn->driverPrivate is initialised to NULL, so we can check if
	 * the allocation has already been done.
	 */
	if (pScrn->driverPrivate != NULL)
		return TRUE;

	pScrn->driverPrivate = xnfcalloc(sizeof(CUBERec), 1);

	/* Initialize it */
	/* No init here yet */
	return TRUE;
}

static void
CUBEFreeRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate == NULL)
		return;
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}


static const OptionInfoRec *
CUBEAvailableOptions(int chipid, int busid)
{
	return CUBEOptions;
}

/* Mandatory */
static void
CUBEIdentify(int flags)
{
	xf86PrintChipsets(CUBE_NAME, "Driver for GameCube/Wii devices ", CUBEChipsets);
}


/* Mandatory */
static Bool
CUBEProbe(DriverPtr drv, int flags)
{
	int i, sst;
	ScrnInfoPtr pScrn;
	GDevPtr *devSections;
	int numDevSections;
	int bus,device,func;
	const char *dev;
	Bool foundScreen = FALSE;

	/* For now, just bail out for PROBE_DETECT. */
	if (flags & PROBE_DETECT)
		return FALSE;

	if ((numDevSections = xf86MatchDevice(CUBE_DRIVER_NAME, &devSections)) <= 0) 
		return FALSE;
	
	for (i = 0; i < numDevSections; i++) {

		 dev = xf86FindOptionValue(devSections[i]->options,"cube");

	 	pScrn = NULL;
		int entity;

		entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
		pScrn = xf86ConfigFbEntity(pScrn,0,entity,
						 NULL,NULL,NULL,NULL);

		if (pScrn) {
			CUBEPtr pCube;
			pScrn->driverVersion = CUBE_VERSION;
			pScrn->driverName = CUBE_DRIVER_NAME;
			pScrn->name = CUBE_NAME;
			pScrn->Probe = CUBEProbe;
			pScrn->PreInit = CUBEPreInit;
			pScrn->ScreenInit = CUBEScreenInit;
			pScrn->EnterVT = CUBEEnterVT;
			pScrn->LeaveVT = CUBELeaveVT;
			pScrn->FreeScreen = CUBEFreeScreen;

			/* Allocate the CUBERec driverPrivate */
			if (!CUBEGetRec(pScrn))
				break;

			pCube = CUBEPTR(pScrn);
			pCube->SST_Index = sst;

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"using %s\n", dev ? dev : "default device");

			foundScreen = TRUE;
		}

	}
	free(devSections);

	return foundScreen;
}


/* Mandatory */
static Bool
CUBEPreInit(ScrnInfoPtr pScrn, int flags)
{
	CUBEPtr pCube;
	MessageType from;
	int ret;
	ClockRangePtr clockRanges;

	if (flags & PROBE_DETECT)
		return FALSE;

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1)
		return FALSE;

	/* Set pScrn->monitor */
	pScrn->monitor = pScrn->confScreen->monitor;

	if (!xf86SetDepthBpp(pScrn, 16, 0, 0, Support32bppFb)) {
		return FALSE;
	}

	/* Check that the returned depth is one we support */
	switch (pScrn->depth) {
		case 16: //We only sopport 16bpp
			/* OK */
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Given depth (%d) is not supported by this driver\n",
			pScrn->depth);
		return FALSE;
	}
	xf86PrintDepthBpp(pScrn);

	/*
	 * This must happen after pScrn->display has been set because
	 * xf86SetWeight references it.
	 */
	if (pScrn->depth > 8) {
		/* The defaults are OK for us */
		rgb zeros = {0, 0, 0};

		if (!xf86SetWeight(pScrn, zeros, zeros)) {
			return FALSE;
		} else {
			/* XXX check that weight returned is supported */
			;
		}
	}

	/* Set the default visual. */
	if (!xf86SetDefaultVisual(pScrn, -1)) {
		return FALSE;
	}
	
	/* We don't support DirectColor at > 8bpp */
	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Given default visual"
		  " (%s) is not supported at depth %d\n",
		  xf86GetVisualName(pScrn->defaultVisual), pScrn->depth);
		return FALSE;
	}

	/* Set default gamma */
	Gamma zeros = {0.0, 0.0, 0.0};

	if (!xf86SetGamma(pScrn, zeros)) {
		return FALSE;
	}


	/* We use a programmable clock */
	pScrn->progClock = TRUE;

	pCube = CUBEPTR(pScrn);

	//time to setup our framebuffer
	initFrameBuffer(pScrn);

	/* Get the entity */
	pCube->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	/* Collect all of the relevant option flags (fill in pScrn->options) */
	xf86CollectOptions(pScrn, NULL);

	/* Process the options */
	if (!(pCube->Options = malloc(sizeof(CUBEOptions))))
		return FALSE;
	memcpy(pCube->Options, CUBEOptions, sizeof(CUBEOptions));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, pCube->Options);

	pCube->OnAtExit = FALSE;
	from = X_DEFAULT;
	if (xf86GetOptValBool(pCube->Options, OPTION_ON_AT_EXIT, &(pCube->OnAtExit)))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, 
				"Cube card will be %s when exiting server.\n", 
				pCube->OnAtExit ? "ON" : "OFF");

	/*
	 * If the user has specified the amount of memory in the XF86Config
	 * file, we respect that setting.
	 */
	if (pCube->pEnt->device->videoRam != 0) {
		pScrn->videoRam = pCube->pEnt->device->videoRam;
		from = X_CONFIG;
	}
	else {
		pScrn->videoRam = pCube->mapped_memlen; 
		from = X_PROBED;
	}

	/* Set up clock ranges so that the xf86ValidateModes() function will not
	 * fail a mode because of the clock requirement (because we don't use the
	 * clock value anyway)
	 */
	clockRanges = xnfcalloc(sizeof(ClockRange), 1);
	clockRanges->next = NULL;
	clockRanges->minClock = 10000;
	clockRanges->maxClock = 300000;
	clockRanges->clockIndex = -1;		/* programmable */
	clockRanges->interlaceAllowed = TRUE;
	clockRanges->doubleScanAllowed = TRUE;

	/* Select valid modes from those available */
	ret = xf86ValidateModes(pScrn, pScrn->monitor->Modes,
				pScrn->display->modes, clockRanges,
				NULL, 256, 2048,
				pScrn->bitsPerPixel, 128, 2048,
				pScrn->display->virtualX,
				pScrn->display->virtualY,
				pScrn->videoRam * 1024,
				LOOKUP_BEST_REFRESH);

	if (ret == -1) {
		CUBEFreeRec(pScrn);
		return FALSE;
	}

	/* Prune the modes marked as invalid */
	xf86PruneDriverModes(pScrn);

	/* If no valid modes, return */
	if (ret == 0 || pScrn->modes == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
		CUBEFreeRec(pScrn);
		return FALSE;
	}

  /* Set the current mode to the first in the list */
  pScrn->currentMode = pScrn->modes;

	/* Do some checking, we will not support a virtual framebuffer larger than
	 * the visible screen. */
  if (pScrn->currentMode->HDisplay != pScrn->virtualX ||
	  pScrn->currentMode->VDisplay != pScrn->virtualY ||
	  pScrn->displayWidth != pScrn->virtualX) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING, 
		  "Virtual size doesn't equal display size. Forcing virtual size to equal display size.\n");
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		  "(Virtual size: %dx%d, Display size: %dx%d)\n", pScrn->virtualX, pScrn->virtualY,
		pScrn->currentMode->HDisplay, pScrn->currentMode->VDisplay);
		/* I'm not entirely sure this is "legal" but I hope so. */
		pScrn->virtualX = pScrn->currentMode->HDisplay;
		pScrn->virtualY = pScrn->currentMode->VDisplay;
		pScrn->displayWidth = pScrn->virtualX;
	}

	/* TODO (From glide driver) : Note: If I return FALSE right here, the server will not restore the
	 * console correctly, forcing a reboot. Must find that. (valid for 3.9Pi)
	 */

	/* Print the list of modes being used */
	xf86PrintModes(pScrn);

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	/* Load fb */
	if (xf86LoadSubModule(pScrn, "fb") == NULL) {
		CUBEFreeRec(pScrn);
		return FALSE;
	}

	/* Load the shadow framebuffer */
	if (!xf86LoadSubModule(pScrn, "shadowfb")) {
		CUBEFreeRec(pScrn);
		return FALSE;
	}

	return TRUE;
}



/*
 * This gets called at the start of each server generation
 */
/* Mandatory */
static Bool
CUBEScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn;
	CUBEPtr pCube;
	int ret;
	VisualPtr visual;

	/*
	 * First get the ScrnInfoRec
	 */
  pScrn = xf86ScreenToScrn(pScreen);

  pCube = CUBEPTR(pScrn);

  if (!CUBEModeInit(pScrn, pScrn->currentMode))
	 return FALSE;

	/*
	 * The next step is to setup the screen's visuals, and initialise the
	 * framebuffer code.  In cases where the framebuffer's default
	 * choices for things like visual layouts and bits per RGB are OK,
	 * this may be as simple as calling the framebuffer's ScreenInit()
	 * function.  If not, the visuals will need to be setup before calling
	 * a fb ScreenInit() function and fixed up after.
	 *
	 * For most PC hardware at depths >= 8, the defaults that fb uses
	 * are not appropriate.  In this driver, we fixup the visuals after.
	 */

	/*
	 * Reset the visual list.
	 */
	miClearVisualTypes();

	/* Setup the visuals we support. Only TrueColor. */
	ret = miSetVisualTypes(pScrn->depth,
							miGetDefaultVisualMask(pScrn->depth),
							pScrn->rgbBits, pScrn->defaultVisual);
	if (!ret)
		return FALSE;

	miSetPixmapDepths ();

	pCube->ShadowPitch =
			((pScrn->virtualX * pScrn->bitsPerPixel >> 3) + 3) & ~3L;
	pCube->ShadowPtr = xnfalloc(pCube->ShadowPitch * pScrn->virtualY);


	/*
	 * Call the framebuffer layer's ScreenInit function, and fill in other
	 * pScreen fields.
	 */
	ret = fbScreenInit(pScreen, pCube->ShadowPtr,
					pScrn->virtualX, pScrn->virtualY,
					pScrn->xDpi, pScrn->yDpi,
					pScrn->displayWidth,
					pScrn->bitsPerPixel);

	if (!ret)
		return FALSE;

	/* Fixup RGB ordering */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			visual->offsetRed = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue = pScrn->offset.blue;
			visual->redMask = pScrn->mask.red;
			visual->greenMask = pScrn->mask.green;
			visual->blueMask = pScrn->mask.blue;
		}
	}

	/* must be after RGB ordering fixed */
	fbPictureInit (pScreen, 0, 0);

	xf86SetBlackWhitePixels(pScreen);
	xf86SetBackingStore(pScreen);

	/* Initialize software cursor.  Must precede creation of the default
	 * colormap */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/* Initialise default colourmap */
	if (!miCreateDefColormap(pScreen))
		return FALSE;

	ShadowFBInit(pScreen, CUBERefreshArea);

	xf86DPMSInit(pScreen, CUBEDisplayPowerManagementSet, 0);

	pScreen->SaveScreen = CUBESaveScreen;

	/* Wrap the current CloseScreen function */
	pCube->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = CUBECloseScreen;

	/* Report any unused options (only for the first generation) */
	if (serverGeneration == 1)
		xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

	/* Done */
	return TRUE;
}



/*
 * This is called when VT switching back to the X server.  Its job is
 * to reinitialise the video mode.
 *
 * We may wish to unmap video/MMIO memory too.
 */
/* Mandatory */
static Bool
CUBEEnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	return CUBEModeInit(pScrn, pScrn->currentMode);
}

/*
 * This is called when VT switching away from the X server.  Its job is
 * to restore the previous (text) mode.
 *
 * We may wish to remap video/MMIO memory too.
 */
/* Mandatory */
static void
CUBELeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	CUBERestore(pScrn, FALSE);
}

/*
 * This is called at the end of each server generation.  It restores the
 * original (text) mode.  It should also unmap the video memory, and free
 * any per-generation data allocated by the driver.  It should finish
 * by unwrapping and calling the saved CloseScreen function.
 */
/* Mandatory */
static Bool
CUBECloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CUBEPtr pCube = CUBEPTR(pScrn);

	if (pScrn->vtSema)
		CUBERestore(pScrn, TRUE);
	free(pCube->ShadowPtr);

	pScrn->vtSema = FALSE;

	pScreen->CloseScreen = pCube->CloseScreen;
	if (pCube->console_fd > 0) {
		/* Unmap the video framebuffer and I/O registers */
		if (pCube->mapped_mem) {
			munmap(pCube->mapped_mem, pCube->mapped_memlen);
			pCube->mapped_mem = NULL;
		}

		close(pCube->console_fd);
		pCube->console_fd = -1;
	}
  return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

/*
 * Free up any persistent data structures
 */
/* Optional */
static void
CUBEFreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	CUBEPtr pCube = CUBEPTR(pScrn);

	/*
	 * This only gets called when a screen is being deleted.  It does not
	 * get called routinely at the end of a server generation.
	 */
	if (pCube && pCube->ShadowPtr)
		free(pCube->ShadowPtr);
	CUBEFreeRec(pScrn);
}

/*
 * Do screen blanking
 */
/* Mandatory */
static Bool
CUBESaveScreen(ScreenPtr pScreen, int mode)
{
	ScrnInfoPtr pScrn;
	CUBEPtr pCube;
	Bool unblank;

	unblank = xf86IsUnblank(mode);
	pScrn = xf86ScreenToScrn(pScreen);
	pCube = CUBEPTR(pScrn);
	pCube->Blanked = !unblank;
	if (unblank)
		CUBERefreshAll(pScrn);
	else
		memset(pCube->mapped_mem,0,pCube->mapped_memlen);

	return TRUE;
}

static Bool
CUBEModeInit(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
	CUBEPtr pCube;
	int ret;
	int width, height;
	double refresh;
	Bool match = FALSE;

	pCube = CUBEPTR(pScrn);

	if (mode->Flags & V_INTERLACE){
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Interlaced modes not supported\n");
		return FALSE;
	}

	width = mode->HDisplay;
	height = mode->VDisplay;

#if 0
	ErrorF("mode->HDisplay = %d, pScrn->displayWidth = %d\n",
		mode->HDisplay, pScrn->displayWidth);
	ErrorF("mode->VDisplay = %d, mode->HTotal = %d, mode->VTotal = %d\n", 
		mode->VDisplay, mode->HTotal, mode->VTotal);
	ErrorF("mode->Clock = %d\n", mode->Clock);
#endif

	if (width == 640 && ((height == 480)||(height == 576)))
	{
		match = TRUE;
	}

	if (!match)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
			"Selected width = %d and height = %d is not supported by "
			"cube/wii\n", width, height);
		return TRUE;
	}

	refresh = (mode->Clock * 1.0e3) / ((double)(mode->HTotal) *
									(double)(mode->VTotal));
#if 0
	ErrorF("Calculated refresh rate for mode is %.2fHz\n",refresh);
#endif


	ret=initFrameBuffer(pScrn);

	if (!ret) {
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "grSstWinOpen returned %d. "
			"You are probably trying to use a resolution that is not "
			"supported by your hardware.", ret);
	 return FALSE;
	}

	memset(pCube->mapped_mem,0,pCube->mapped_memlen);
	pCube->Blanked = FALSE;
	pCube->CubeInitiated = TRUE;
	return TRUE;
}

static void
CUBERestore(ScrnInfoPtr pScrn, Bool Closing)
{
	CUBEPtr pCube;

	pCube = CUBEPTR(pScrn);

	if (!(pCube->CubeInitiated))
		return;
	pCube->CubeInitiated = FALSE;
	pCube->Blanked = TRUE;
	memset(pCube->mapped_mem,0,pCube->mapped_memlen);
}



static int
initFrameBuffer(ScrnInfoPtr pScrn)
{
	CUBEPtr pCube;
	pCube = CUBEPTR(pScrn);

	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	const char *fbdev;

	initRGB2YUVTables();

	/* Initialize the library */
	fbdev = "/dev/fb0";

	pCube->console_fd = open(fbdev, O_RDWR, 0);
	if (pCube->console_fd < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Unable to open %s", fbdev);
		return (-1);
	}

	/* Get the type of video hardware */
	if (ioctl(pCube->console_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Couldn't get console hardware info");
		return (-1);
	}
	switch (finfo.type) {
	case FB_TYPE_PACKED_PIXELS:
		/* Supported, no worries.. */
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Unsupported console hardware");
		return (-1);
	}
	switch (finfo.visual) {
	case FB_VISUAL_TRUECOLOR:
		/* Supported, no worries.. */
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Unsupported console hardware");
		return (-1);
	}

	/* Memory map the device, compensating for buggy PPC mmap() */
	pCube->mapped_offset = (((long)finfo.smem_start) -
			 (((long)finfo.smem_start) & ~(sysconf(_SC_PAGESIZE) - 1)));
	pCube->mapped_memlen = finfo.smem_len + pCube->mapped_offset;
	pCube->mapped_mem = mmap(NULL, pCube->mapped_memlen,
			  PROT_READ | PROT_WRITE, MAP_SHARED, pCube->console_fd, 0);
	if (pCube->mapped_mem == (char *)-1) {
	  xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Unable to memory map the video hardware");
		pCube->mapped_mem = NULL;
		return (-1);
	}

	/* Determine the current screen depth */
	if (ioctl(pCube->console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Couldn't get console pixel format");
		return (-1);
	}


	
	vinfo.activate = FB_ACTIVATE_NOW;
	vinfo.accel_flags = 0;	/* Temporarily reserve registers */
	vinfo.bits_per_pixel = 16;
	vinfo.xres = pScrn->virtualX;
	vinfo.xres_virtual = pScrn->virtualX;
	vinfo.yres = pScrn->virtualY;
	vinfo.yres_virtual = pScrn->virtualX;
	vinfo.xoffset = 0;
	vinfo.yoffset = 0;
	vinfo.red.length = vinfo.red.offset = 0;
	vinfo.green.length = vinfo.green.offset = 0;
	vinfo.blue.length = vinfo.blue.offset = 0;
	vinfo.transp.length = vinfo.transp.offset = 0;

	ioctl(pCube->console_fd, FBIOPUT_VSCREENINFO, &vinfo);

	
  return 1;
}
//static void GC_UpdateRectRGB16(_THIS, SDL_Rect * rect, int pitch)
static void CUBERefreshArea(ScrnInfoPtr pScrn, int num, BoxPtr pbox)
{
	CUBEPtr pCube = CUBEPTR(pScrn);

	int width, height, left, i, mod, mod32;
	u8 *src, *dst;
	u32 *src32, *dst32;
	u16 *rgb;

	if (pCube->Blanked)
		return;

	/* XXX case width < 2 needs special treatment */
	while(num--)
	{
		/* in pixel units */
		left = pbox->x1 & ~1;	/* 2 pixel align */
		width = (pbox->x2 + 1) & ~1;	/* 2 pixel align in excess */
		height = pbox->y2-pbox->y1;

		/* in bytes, src and dest are 16bpp */
		//src = _this->hidden->buffer + (rect->y * pitch) + left * 2;
		src=pCube->ShadowPtr + (pbox->y1 * pCube->ShadowPitch) + 
		(left * 2);
		dst = pCube->mapped_mem + (pbox->y1 * pCube->ShadowPitch) + left * 2;
		mod = pCube->ShadowPitch - width * 2;

		src32 = (u32 *) src;
		dst32 = (u32 *) dst;
		mod32 = mod / 4;

		while (height--) {
			i = width / 2;

			while (i--) {
				rgb = (u16 *) src32;
				*dst32++ = rgbrgb16toyuy2(rgb[0], rgb[1]);
				src32++;
			}
			dst32 += mod32;
			src32 += mod32;
		}
		pbox++;
	}
}

/*
 * CUBEDisplayPowerManagementSet --
 *
 * Sets VESA Display Power Management Signaling (DPMS) Mode.
 */
static void
CUBEDisplayPowerManagementSet(ScrnInfoPtr pScrn, int PowerManagementMode,
					 int flags)
{
	CUBEPtr pCube = CUBEPTR(pScrn);
	static int oldmode = -1;

#if 0
	ErrorF("CUBEDisplayPowerManagementSet: %d\n", PowerManagementMode);
#endif

	if (oldmode == DPMSModeOff && PowerManagementMode != DPMSModeOff)
		CUBEModeInit(pScrn, pScrn->currentMode);

	switch (PowerManagementMode){
	case DPMSModeOn:
		/* Screen: On; HSync: On, VSync: On */
		pCube->Blanked = FALSE;
		CUBERefreshAll(pScrn);
		break;
	case DPMSModeStandby:
	case DPMSModeSuspend:
		pCube->Blanked = TRUE;
		memset(pCube->mapped_mem,0,pCube->mapped_memlen);
	break;
		case DPMSModeOff:
		CUBERestore(pScrn, FALSE);
	break;
	}
	oldmode = PowerManagementMode;
}


static void
CUBERefreshAll(ScrnInfoPtr pScrn)
{
	BoxRec box;
	box.x1 = 0;
	box.x2 = 640; /*Hardcoded due to bugs, once fixed replace with: pScrn->currentMode->HDisplay;*/
	box.y1 = 0;
	box.y2 = pScrn->currentMode->VDisplay;
	CUBERefreshArea(pScrn, 1, &box);
}
