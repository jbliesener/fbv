#include "config.h"
#include "fbv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <termios.h>
#include <signal.h>


#define PAN_STEPPING 20

static int opt_clear = 1;
static int opt_alpha = 0;
static int opt_hide_cursor = 1;
static int opt_image_info = 1;
static int opt_shrink = 0;
static int opt_widthonly = 0;
static int opt_heightonly = 0;
static int opt_smartfit = -1;
static int opt_delay = 0;
static int opt_enlarge = 0;
static int opt_ignore_aspect = 0;
static int opt_orientation = 0;
static int opt_skip_tty = 0;
static int opt_color_remap = 0;
static float opt_color_points[12];
static float color_bezier_points[12] = {
	0.0, 1.0/3.0, 2.0/3.0, 1.0,
	0.0, 1.0/3.0, 2.0/3.0, 1.0,
	0.0, 1.0/3.0, 2.0/3.0, 1.0
};
static unsigned char color_map[3 * 256];

static char *imagename = NULL;

static char inline_status[] =
	"\nviewer status:\n"
	"%cshrink(f)%c          %cquality shrin(k)%c    %c(e)nlarge%c\n"
	"%chorizonta(l)_fit%c   %cver(t)ical_fit%c      %caspect(i)%c\n"
	" zoom:%g\n";
static char opener[2] = {' ', '['};
static char closer[2] = {' ', ']'};

static char inline_help[] =
	"keys:\n"
	"r		Redraw the image\n"
	"< or ,		Previous image\n"
	"> or .		Next image\n"
	"a, d, w, x	Scroll the image (cursor keys also do that)\n"
	"f		Toggle shrinking on/off\n"
	"k		Toggle shrinking quality\n"
	"e		Toggle enlarging on/off\n"
	"l		Toggle fitting the image horizontally\n"
	"t		Toggle fitting the image vertically\n"
	"i		Toggle respecting the image aspect on/off\n"
	"+, -, 0		Increase, decrease and reset zoom\n"
	"n		Rotate the image 90 degrees left\n"
	"m		Rotate the image 90 degrees right\n"
	"p		Disable all transformations\n"
	"h		Help and image information\n";

void setup_console(int t)
{
	struct termios our_termios;
	static struct termios old_termios;

	if (isatty(fileno(stdout)))
	{
		if(t)
		{
			printf("setup console\n");
			tcgetattr(0, &old_termios);
			memcpy(&our_termios, &old_termios, sizeof(struct termios));
			our_termios.c_lflag &= !(ECHO | ICANON);
			tcsetattr(0, TCSANOW, &our_termios);
		}
		else
		{
			//printf("restore console\n");
			tcsetattr(0, TCSANOW, &old_termios);
		}
	}
	else
	{
		printf("stdout is not connected to a terminal\n");
	}
 }

static inline void do_rotate(struct image *i, int rot)
{
	if(rot)
	{
		unsigned char *image, *alpha = NULL;
		int t;

		image = rotate(i->rgb, i->width, i->height, rot);
		if(i->alpha)
			alpha = alpha_rotate(i->alpha, i->width, i->height, rot);
		if(i->do_free)
		{
			free(i->alpha);
			free(i->rgb);
		}

		i->rgb = image;
		i->alpha = alpha;
		i->do_free = 1;

		if(rot & 1)
		{
			t = i->width;
			i->width = i->height;
			i->height = t;
		}
	}
}


static inline void do_enlarge(struct image *i, int screen_width, int screen_height, int ignoreaspect, int widthonly, int heightonly)
{
	if(((i->width > screen_width) || (i->height > screen_height)) && (!ignoreaspect))
		return;
	if((i->width < screen_width) || (i->height < screen_height))
	{
		int xsize = i->width, ysize = i->height;
		unsigned char * image, * alpha = NULL;

		if(ignoreaspect)
		{
			if(i->width < screen_width)
				xsize = screen_width;
			if(i->height < screen_height)
				ysize = screen_height;

			goto have_sizes;
		}

		if(widthonly) {
			xsize = screen_width;
			ysize = i->height * screen_width / i->width;
			goto have_sizes;
		}

		if(heightonly) {
			xsize = i->width * screen_height / i->height;
			ysize = screen_height;
			goto have_sizes;
		}

		if((i->height * screen_width / i->width) <= screen_height)
		{
			xsize = screen_width;
			ysize = i->height * screen_width / i->width;
			goto have_sizes;
		}

		if((i->width * screen_height / i->height) <= screen_width)
		{
			xsize = i->width * screen_height / i->height;
			ysize = screen_height;
			goto have_sizes;
		}
		return;
have_sizes:
		image = simple_resize(i->rgb, i->width, i->height, xsize, ysize);
		if(i->alpha)
			alpha = alpha_resize(i->alpha, i->width, i->height, xsize, ysize);

		if(i->do_free)
		{
			free(i->alpha);
			free(i->rgb);
		}

		i->rgb = image;
		i->alpha = alpha;
		i->do_free = 1;
		i->width = xsize;
		i->height = ysize;
	}
}


static inline void do_fit_to_screen(struct image *i, int screen_width, int screen_height, int ignoreaspect, int widthonly, int heightonly, int cal)
{
	if((i->width > screen_width) || (i->height > screen_height))
	{
		unsigned char * new_image, * new_alpha = NULL;
		int nx_size = i->width, ny_size = i->height;

		if(ignoreaspect)
		{
			if(i->width > screen_width)
				nx_size = screen_width;
			if(i->height > screen_height)
				ny_size = screen_height;
		}
		else if(widthonly) {
			nx_size = screen_width;
			ny_size = i->height * screen_width / i->width;
		}
		else if(heightonly) {
			nx_size = i->width * screen_height / i->height;
			ny_size = screen_height;
		}
		else
		{
			if((i->height * screen_width / i->width) <= screen_height)
			{
				nx_size = screen_width;
				ny_size = i->height * screen_width / i->width;
			}
			else
			{
				nx_size = i->width * screen_height / i->height;
				ny_size = screen_height;
			}
		}

		if(cal)
			new_image = color_average_resize(i->rgb, i->width, i->height, nx_size, ny_size);
		else
			new_image = simple_resize(i->rgb, i->width, i->height, nx_size, ny_size);

		if(i->alpha)
			new_alpha = alpha_resize(i->alpha, i->width, i->height, nx_size, ny_size);

		if(i->do_free)
		{
			free(i->alpha);
			free(i->rgb);
		}

		i->rgb = new_image;
		i->alpha = new_alpha;
		i->do_free = 1;
		i->width = nx_size;
		i->height = ny_size;
	}
}


int show_image(char *filename)
{
	int (*load)(char *, unsigned char *, unsigned char **, int, int);

	unsigned char * image = NULL;
	unsigned char * alpha = NULL;

	int c, ret;
	int x_size, y_size, screen_width, screen_height;
	int x_pan, y_pan, x_offs, y_offs, refresh = 1;
	int delay = opt_delay, retransform = 1, noshow = 0;

	int transform_shrink = opt_shrink, transform_enlarge = opt_enlarge;
	int transform_cal = (opt_shrink == 2), transform_iaspect = opt_ignore_aspect;
	int transform_rotation = 0;
	int transform_widthonly = opt_widthonly, transform_heightonly = opt_heightonly;

	double zoom = 1;

	struct image i;

#ifdef FBV_SUPPORT_PNG
	if(fh_png_id(filename))
	if(fh_png_getsize(filename, &x_size, &y_size) == FH_ERROR_OK)
	{
		load = fh_png_load;
		goto identified;
	}
#endif

#ifdef FBV_SUPPORT_JPEG
	if(fh_jpeg_id(filename))
	if(fh_jpeg_getsize(filename, &x_size, &y_size) == FH_ERROR_OK)
	{
		load = fh_jpeg_load;
		goto identified;
	}
#endif

#ifdef FBV_SUPPORT_BMP
	if(fh_bmp_id(filename))
	if(fh_bmp_getsize(filename, &x_size, &y_size) == FH_ERROR_OK)
	{
		load = fh_bmp_load;
		goto identified;
	}
#endif
	fprintf(stderr, "%s: Unable to access file or file format unknown.\n", filename);
	return(1);

identified:

	if(!(image = (unsigned char*)malloc(x_size * y_size * 3)))
	{
		fprintf(stderr, "%s: Out of memory.\n", filename);
		goto error;
	}

	if(load(filename, image, &alpha, x_size, y_size) != FH_ERROR_OK)
	{
		fprintf(stderr, "%s: Image data is corrupt?\n", filename);
		goto error;
	}

	if(!opt_alpha)
	{
		free(alpha);
		alpha = NULL;
	}

	if(getCurrentRes(&screen_width, &screen_height))
		goto error;
	i.do_free = 0;

	if (opt_smartfit>=0)
	{
		transform_shrink = 1;
		transform_enlarge = 1;

		// Check if screen aspect ratio is larger than image aspect ratio
		// screen_width/screen_height > x_size/y_size
		if (screen_width*y_size > x_size*screen_height)
		{
			if (opt_smartfit>100-100*x_size*screen_height/(y_size*screen_width))
			{
				transform_widthonly = 1;
				transform_heightonly = 0;
			}
		}
		else
		{
			if (opt_smartfit>100-100*y_size*screen_width/(x_size*screen_height))
			{
				transform_widthonly = 0;
				transform_heightonly = 1;
			}
		}
	}

	if(opt_orientation)
	{
		transform_rotation = opt_orientation;
	}


	while(1)
	{
		if(retransform)
		{
			if(i.do_free)
			{
				free(i.rgb);
				free(i.alpha);
			}
			i.width = x_size;
			i.height = y_size;
			i.rgb = image;
			i.alpha = alpha;
			i.do_free = 0;

			if(transform_rotation) {
				do_rotate(&i, transform_rotation);
			}

			if(zoom > 1) {
				do_fit_to_screen(&i, x_size / zoom, y_size / zoom, 0, 0, 0, 0);
			}
			if(zoom < 1) {
				do_enlarge(&i, x_size / zoom, y_size / zoom, 0, 0, 0);
			}

			if(transform_shrink) {
				do_fit_to_screen(&i, screen_width, screen_height, transform_iaspect, transform_widthonly, transform_heightonly, transform_cal);
			}

			if(transform_enlarge) {
				do_enlarge(&i, screen_width, screen_height, transform_iaspect, transform_widthonly, transform_heightonly);
			}

			x_pan = y_pan = 0;
			if (opt_smartfit>=0)
			{
				if (i.width>screen_width)
				{
					x_pan = (i.width-screen_width)/2;
				}

				if (i.height>screen_height)
				{
					y_pan = (i.height-screen_height)/2;
				}
			}

			if (opt_color_remap) {
				int cnt;
				unsigned char *px = i.rgb;
				for (cnt=0; cnt<i.width * i.height; cnt++) {
					*px     = color_map[*px];
					*(px+1) = color_map[256 + *(px+1)];
					*(px+2) = color_map[512 + *(px+2)];
					px += 3;
				}
			}

			refresh = 1; retransform = 0;
			if(opt_clear)
			{
				printf("\033[H\033[J");
				fflush(stdout);
			}
			if(opt_image_info) {
				printf("fbv - The Framebuffer Viewer\n");
				printf("%s\n", imagename ? imagename : filename);
				printf("%d x %d\n", x_size, y_size);
				printf(inline_status,
					opener[transform_shrink],
					closer[transform_shrink],
					opener[transform_cal],
					closer[transform_cal],
					opener[transform_enlarge],
					closer[transform_enlarge],
					opener[transform_widthonly],
					closer[transform_widthonly],
					opener[transform_heightonly],
					closer[transform_heightonly],
					opener[transform_iaspect],
					closer[transform_iaspect],
					1 / zoom
				);

				if (isatty(fileno(stdout)))
					printf("\n%s", inline_help);
			}
		}
		if(refresh && !noshow)
		{
			if(i.width < screen_width)
				x_offs = (screen_width - i.width) / 2;
			else
				x_offs = 0;

			if(i.height < screen_height)
				y_offs = (screen_height - i.height) / 2;
			else
				y_offs = 0;

			if(fb_display(i.rgb, i.alpha, i.width, i.height, x_pan, y_pan, x_offs, y_offs))
				goto error;
			refresh = 0;
		}
		if(delay)
		{
			if (isatty(fileno(stdin)))
			{
				struct timeval tv;
				fd_set fds;
				tv.tv_sec = delay / 10;
				tv.tv_usec = (delay % 10) * 100000;
				FD_ZERO(&fds);
				FD_SET(0, &fds);
				if(select(1, &fds, NULL, NULL, &tv) <= 0)
					{
						ret = 1;
						break;
					}
				delay = 0;
			}
			else {
				usleep(delay*100);
				ret = 1;
				break;
			}
		}

		if (isatty(fileno(stdin)) && !opt_skip_tty)
		{
			c = getchar();
			if (c == -1)
				c = 'r';
			switch(c)
			{
				case EOF:
				case 'q':
					ret = 0;
					goto done;
				case ' ': case 10: case 13:
					goto done;
				case '>': case '.':
					ret = 1;
					goto done;
				case '<': case ',':
					ret = -1;
					goto done;
				case 'r':
					refresh = 1;
					break;
				case 'a': case 'D':
					if(x_pan == 0) break;
					x_pan -= i.width / PAN_STEPPING;
					if(x_pan < 0) x_pan = 0;
					refresh = 1;
					break;
				case 'd': case 'C':
					if(x_offs) break;
					if(x_pan >= (i.width - screen_width)) break;
					x_pan += i.width / PAN_STEPPING;
					if(x_pan > (i.width - screen_width)) x_pan = i.width - screen_width;
					refresh = 1;
					break;
				case 'w': case 'A':
					if(y_pan == 0) break;
					y_pan -= i.height / PAN_STEPPING;
					if(y_pan < 0) y_pan = 0;
					refresh = 1;
					break;
				case 'x': case 'B':
					if(y_offs) break;
					if(y_pan >= (i.height - screen_height)) break;
					y_pan += i.height / PAN_STEPPING;
					if(y_pan > (i.height - screen_height)) y_pan = i.height - screen_height;
					refresh = 1;
					break;
				case 'f':
					transform_shrink = !transform_shrink;
					retransform = 1;
					break;
				case 'e':
					transform_enlarge = !transform_enlarge;
					retransform = 1;
					break;
				case 'l':
					transform_widthonly = !transform_widthonly;
					transform_heightonly = 0;
					retransform = 1;
					break;
				case 't':
					transform_widthonly = 0;
					transform_heightonly = !transform_heightonly;
					retransform = 1;
					break;
				case 'k':
					transform_cal = !transform_cal;
					retransform = 1;
					break;
				case 'i':
					transform_iaspect = !transform_iaspect;
					retransform = 1;
					break;
				case '0':
				case '+':
				case '-':
					transform_cal = 0;
					transform_iaspect = 0;
					transform_enlarge = 0;
					transform_shrink = 0;
					transform_widthonly = 0;
					transform_heightonly = 0;
					zoom = c == '0' ? 1 : c == '+' ? zoom / 1.5 : zoom * 1.5;
					retransform = 1;
					break;
				case 'p':
					transform_cal = 0;
					transform_iaspect = 0;
					transform_enlarge = 0;
					transform_shrink = 0;
					transform_widthonly = 0;
					transform_heightonly = 0;
					zoom = 1;
					retransform = 1;
					break;
				case 'n':
					transform_rotation -= 1;
					if(transform_rotation < 0)
						transform_rotation += 4;
					retransform = 1;
					break;
				case 'm':
					transform_rotation += 1;
					if(transform_rotation > 3)
						transform_rotation -= 4;
					retransform = 1;
					break;
				case 'h': case '\033':
					if(c == '\033' && !noshow)
						break;
					if(!opt_image_info)
						break;
					retransform = 1;
					noshow = !noshow;
					break;
			}
		}
		else
		{
			// Non-interactive, exit immediately
			ret = 1;
			break;
		}
	}// while(1)

done:
	if(opt_clear)
	{
		printf("\033[H\033[J");
		fflush(stdout);
	}

error:
	free(image);
	free(alpha);
	if(i.do_free)
	{
		free(i.rgb);
		free(i.alpha);
	}
	return ret;
}


void build_colormap() {
	// bezier spline calculation
	float r0 = color_bezier_points[0];
	float r1 = color_bezier_points[1];
	float r2 = color_bezier_points[2];
	float r3 = color_bezier_points[3];
	float g0 = color_bezier_points[4];
	float g1 = color_bezier_points[5];
	float g2 = color_bezier_points[6];
	float g3 = color_bezier_points[7];
	float b0 = color_bezier_points[8];
	float b1 = color_bezier_points[9];
	float b2 = color_bezier_points[10];
	float b3 = color_bezier_points[11];
	int x;
	for (x = 0; x<256; x++) {
		float t = x / 255.0;
		float t1 = 1.0-t;
		float t2 = t*t;
		float t3 = t2 * t;
		float t12 = t1*t1;
		float t13 = t12*t;
		float r = (t13*r0+t12*t*r1+t1*t2*r2+t3*r3)*255.0;
		float g = (t13*g0+t12*t*g1+t1*t2*g2+t3*g3)*255.0;
		float b = (t13*b0+t12*t*b1+t1*t2*b2+t3*b3)*255.0;	
		if (r < 0.0)   r = 0.0;
		if (r > 255.0) r = 255.0;
		if (g < 0.0)   g = 0.0;
		if (g > 255.0) g = 255.0;
		if (b < 0.0)   b = 0.0;
		if (b > 255.0) b = 255.0;
		color_map[x]       = (unsigned char) r;
		color_map[x + 256] = (unsigned char) g;
		color_map[x + 512] = (unsigned char) b;
	}
}

void help(char *name)
{
	printf("Usage: %s [options] image1 image2 image3 ...\n\n"
		   "Available options:\n"
		   "  -h, --help          Show this help\n"
		   "  -a, --alpha         Use the alpha channel (if applicable)\n"
		   "  -c, --dontclear     Do not clear the screen before and after displaying the image\n"
		   "  -u, --donthide      Do not hide the cursor before and after displaying the image\n"
		   "  -i, --noinfo        Supress image information\n"
		   "  -f, --shrink        Shrink (using a simple resizing routine) the image to fit onto screen if necessary\n"
		   "  -k, --colorshrink   Shrink (using a 'color average' resizing routine) the image to fit onto screen if necessary\n"
		   "  -e, --enlarge       Enlarge the image to fit the whole screen if necessary\n"
		   "  -l, --widthonly     Fit the image horizontally\n"
		   "  -t, --heightonly    Fit the image vertically\n"
		   "  -x <percent>, --smartfit <percent>  Show image by covering the whole screen if less than <percent>%% is out of screen\n"
		   "  -r, --ignore-aspect Ignore the image aspect while resizing\n"
		   "  -s <delay>, --delay <d>  Slideshow, 'delay' is the slideshow delay in tenths of seconds.\n\n"
		   "  -n imagename(s)     Image name(s) shown in help\n"
		   "  -o <mode>, --orientation <mode>  Show image in specific orientation (0 = no rotation, 1 = 90° rotation, 2 = 180° rotation, 3 = 270° rotation)\n"
		   "  -y, --skiptty		  Shows the image only once and skips tty input mode.\n"
		   "  -m, --color <correct>   Specify 1, 3, 4, 6 or 12 color correction points.\n"
		   "Input keys:\n"
		   " r          : Redraw the image\n"
		   " < or ,     : Previous image\n"
		   " > or .     : Next image\n"
		   " a, d, w, x : Pan the image\n"
		   " f          : Toggle resizing on/off\n"
		   " k          : Toggle resizing quality\n"
		   " e          : Toggle enlarging on/off\n"
		   " l          : Toggle fitting the image horizontally\n"
		   " t          : Toggle fitting the image vertically\n"
		   " i          : Toggle respecting the image aspect on/off\n"
		   " +, -, 0    : Increase, decrease and reset zoom\n"
		   " n          : Rotate the image 90 degrees left\n"
		   " m          : Rotate the image 90 degrees right\n"
		   " p          : Disable all transformations\n"
		   " h          : Help and image information\n"
		   " Copyright (C) 2000 - 2004 Mateusz Golicz, Tomasz Sterna.\n"
		   " Copyright (C) 2013 yanlin, godspeed1989@gitbub\n", name);
}

void sighandler(int s)
{
	if(opt_hide_cursor)
	{
		printf("\033[?25h");
		fflush(stdout);
	}
	setup_console(0);
	_exit(128 + s);
}

int main(int argc, char **argv)
{
	static struct option long_options[] =
	{
		{"help",          no_argument,  0, 'h'},
		{"noclear",       no_argument,  0, 'c'},
		{"alpha",         no_argument,  0, 'a'},
		{"unhide",        no_argument,  0, 'u'},
		{"noinfo",        no_argument,  0, 'i'},
		{"shrink",        no_argument,  0, 'f'},
		{"colorshrink",   no_argument,  0, 'k'},
		{"delay",         required_argument, 0, 's'},
		{"enlarge",       no_argument,  0, 'e'},
		{"widthonly",     no_argument,  0, 'l'},
		{"heightonly",    no_argument,  0, 't'},
		{"smartfit",     required_argument,  0, 'x'},
		{"ignore-aspect", no_argument,  0, 'r'},
		{"orientation",		  required_argument,  0, 'o'},
		{"skiptty",		  required_argument,  0, 'y'},
		{"imagename",     required_argument, 0, 'n'},
		{"color",         required_argument, 0, 'm'},
		{0, 0, 0, 0}
	};
	int c, i, j;
	char *nameopts = NULL;
	char **namestarts = NULL;
	char *pch;
	float slope;

	if(argc < 2)
	{
		help(argv[0]);
		fprintf(stderr, "Error: Required argument missing.\n");
		return 1;
	}

	while((c = getopt_long_only(argc, argv, "hn:cauifks:eltxro:ym:", long_options, NULL)) != EOF)
	{
		switch(c)
		{
			case 'a':
				opt_alpha = 1;
				break;
			case 'c':
				opt_clear = 0;
				break;
			case 's':
				opt_delay = atoi(optarg);
				break;
			case 'u':
				opt_hide_cursor = 0;
				break;
			case 'n':
				nameopts = optarg;
				break;
			case 'h':
				help(argv[0]);
				return(0);
			case 'i':
				opt_image_info = 0;
				break;
			case 'f':
				opt_shrink = 1;
				break;
			case 'k':
				opt_shrink = 2;
				break;
			case 'e':
				opt_enlarge = 1;
				break;
			case 'l':
				opt_widthonly = 1;
				break;
			case 't':
				opt_heightonly = 1;
				break;
			case 'x':
				opt_smartfit = atoi(optarg);
				break;
			case 'r':
				opt_ignore_aspect = 1;
				break;
			case 'o':
				opt_orientation = atoi(optarg);
				break;
			case 'y':
				opt_skip_tty = 1;
				break;
			case 'm':
				i=0;
				pch = strtok (optarg,",");
				while (pch != NULL)
				{
					opt_color_points[i] = atof(pch);
					// bound value
					if (opt_color_points[i]<0.0)
						opt_color_points[i] = 0.0;
					if (opt_color_points[i]>1.0)
						opt_color_points[i] = 1.0;
					i++;
					pch = strtok (NULL, ",");
					if (i>=12) 
						break;
				}
				/* 
				printf("%d points:\n",i);
				for (j=0; j<i; j++) {
					printf("  %d: %f\n", j, opt_color_points[j]);
				}
				*/
				switch (i) {
					case 1:
						// single point: uniform value scaling
						color_bezier_points[0] = color_bezier_points[4] = color_bezier_points[8]  = 0.0;
						color_bezier_points[3] = color_bezier_points[7] = color_bezier_points[11] = opt_color_points[0];
						color_bezier_points[1] = color_bezier_points[5] = color_bezier_points[9]  = opt_color_points[0] / 3.0;
						color_bezier_points[2] = color_bezier_points[6] = color_bezier_points[10] = color_bezier_points[1] * 2.0;
						opt_color_remap = 1;
						break;
					case 2:
						// two points: uniform slope + offset
						color_bezier_points[0] = color_bezier_points[4] = color_bezier_points[8]  = opt_color_points[0];
						color_bezier_points[3] = color_bezier_points[7] = color_bezier_points[11] = opt_color_points[1];
						slope = (opt_color_points[1] - opt_color_points[0]) / 3.0;
						color_bezier_points[1] = color_bezier_points[5] = color_bezier_points[9]  = opt_color_points[0] + slope;
						color_bezier_points[2] = color_bezier_points[6] = color_bezier_points[10] = opt_color_points[0] + slope * 2.0;
						opt_color_remap = 1;
						break;
					case 3:
						// three points: distinct scaling for each channel
						color_bezier_points[0]  = 0.0;
						color_bezier_points[3]  = opt_color_points[0]; 
						color_bezier_points[1]  = opt_color_points[0] / 3.0; 
						color_bezier_points[2]  = color_bezier_points[1] * 2.0;
						color_bezier_points[4]  = 0.0;
						color_bezier_points[7]  = opt_color_points[1]; 
						color_bezier_points[5]  = opt_color_points[1] / 3.0; 
						color_bezier_points[6]  = color_bezier_points[5] * 2.0;
						color_bezier_points[8]  = 0.0;
						color_bezier_points[11] = opt_color_points[2]; 
						color_bezier_points[9]  = opt_color_points[2] / 3.0; 
						color_bezier_points[10] = color_bezier_points[9] * 2.0;
						opt_color_remap = 1;
						break;
					case 4:
						// four points: uniform bezier curve for all channels
						color_bezier_points[0] = color_bezier_points[4] = color_bezier_points[8]  = opt_color_points[0];
						color_bezier_points[3] = color_bezier_points[7] = color_bezier_points[11] = opt_color_points[1];
						color_bezier_points[1] = color_bezier_points[5] = color_bezier_points[9]  = opt_color_points[2];
						color_bezier_points[2] = color_bezier_points[6] = color_bezier_points[10] = opt_color_points[3];
						opt_color_remap = 1;
						break;
					case 6:
						// six points: slope and offset for each channel
						color_bezier_points[0]  = opt_color_points[0];
						color_bezier_points[3]  = opt_color_points[1]; 
						slope = (opt_color_points[1] - opt_color_points[0]) / 3.0;
						color_bezier_points[1]  = opt_color_points[0] + slope; 
						color_bezier_points[2]  = opt_color_points[0] + slope * 2.0;
						color_bezier_points[4]  = opt_color_points[2];
						color_bezier_points[7]  = opt_color_points[3]; 
						slope = (opt_color_points[3] - opt_color_points[2]) / 3.0;
						color_bezier_points[5]  = opt_color_points[2] + slope; 
						color_bezier_points[6]  = opt_color_points[2] + slope * 2.0;
						color_bezier_points[8]  = opt_color_points[4];
						color_bezier_points[11] = opt_color_points[5]; 
						slope = (opt_color_points[5] - opt_color_points[4]) / 3.0;
						color_bezier_points[9]  = opt_color_points[4] + slope; 
						color_bezier_points[10] = opt_color_points[4] + slope * 2.0;
						opt_color_remap = 1;
						break;
					case 12:
						// 12 points: full bezier parameters
						color_bezier_points[0]  = opt_color_points[0];
						color_bezier_points[1]  = opt_color_points[1];
						color_bezier_points[2]  = opt_color_points[2];
						color_bezier_points[3]  = opt_color_points[3];
						color_bezier_points[4]  = opt_color_points[4];
						color_bezier_points[5]  = opt_color_points[5];
						color_bezier_points[6]  = opt_color_points[6];
						color_bezier_points[7]  = opt_color_points[7];
						color_bezier_points[8]  = opt_color_points[8];
						color_bezier_points[9]  = opt_color_points[9];
						color_bezier_points[10] = opt_color_points[10];
						color_bezier_points[11] = opt_color_points[11];
						opt_color_remap = 1;
						break;
					default:
						printf("Please specify either 1, 2, 3, 4, 6 or 12 points between 0.0 and 1.0 for color correction:\n");
						printf("   1 point:  uniform contrast adjustment for all channels\n");
						printf("   2 points: uniform contrast and brightness adjustment for all channels\n");
						printf("   3 points: individual contrast adjustment for each channel\n");
						printf("   4 points: uniform bezier curve adjustment for all channels\n");
						printf("   6 points: individual contrast and brightness adjustment for each channel\n");
						printf("  12 points: individual bezier curve adjustment for each channel\n");
						break;
				}
		}
	}

	if (opt_color_remap) {
		build_colormap();
	}

	if(!argv[optind])
	{
		fprintf(stderr, "Required argument missing! Consult %s -h.\n", argv[0]);
		return 1;
	}

	if (argc - optind > 1) {
		namestarts = (char **) malloc((argc - optind) * sizeof(char *));
		for (i = 0; i < argc - optind; i++) {
			namestarts[i] = nameopts;
			if (nameopts == NULL)
				continue;
			nameopts = strchr(nameopts, '^');
			if (nameopts == NULL)
				continue;
			*nameopts = '\0';
			nameopts++;
		}
	}

	signal(SIGHUP, sighandler);
	signal(SIGINT, sighandler);
	signal(SIGQUIT, sighandler);
	signal(SIGSEGV, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGABRT, sighandler);

	vt_setup();

	if(opt_hide_cursor)
	{
		printf("\033[?25l");
		fflush(stdout);
	}

	setup_console(1);

	i = optind;
	while(argv[i])
	{
		imagename = (argc - optind == 1) ?
			 nameopts : namestarts[i - optind];
		int r = show_image(argv[i]);
		if(r == 0)
			break;
		i += r;
		if(i < optind)
			i = optind;
	}

	setup_console(0);

	if(opt_hide_cursor)
	{
		printf("\033[?25h");
		fflush(stdout);
	}

	if(opt_orientation && (opt_orientation > 3 || opt_orientation < 0))
		opt_orientation = 0;

	return 0;
}

