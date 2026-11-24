/*
 * whet: the Whetstone benchmark's modules (Curnow and Wichmann), double,
 * for cpubench's bench16: -m16 -ffast-math, so x87 code (fsin, fcos, fpatan,
 * fsqrt, f2xm1, fyl2x) under 66h/67h prefixes in real mode, as CheckIt's
 * Whetstone. Entered from whet_start.S; a checksum of the results in EAX.
 */
#define LOOPS 4

static double t = 0.499975, t1 = 0.50025, t2 = 2.0;
static double e1[4];
static int j, k, l;

static void pa(double *e)
{
	for (int i = 0; i < 6; i++) {
		e[0] = (e[0] + e[1] + e[2] - e[3]) * t;
		e[1] = (e[0] + e[1] - e[2] + e[3]) * t;
		e[2] = (e[0] - e[1] + e[2] + e[3]) * t;
		e[3] = (-e[0] + e[1] + e[2] + e[3]) / t2;
	}
}

static void p3(double x, double y, double *z)
{
	double x1 = t * (x + y), y1 = t * (x1 + y);
	*z = (x1 + y1) / t2;
}

static void p0(void)
{
	e1[j] = e1[k];
	e1[k] = e1[l];
	e1[l] = e1[j];
}

static double h(double v) { return v * 1000003.0; }

unsigned whet_main(void)
{
	double x1, x2, x3, x4, x, y, z, sum = 0;
	for (int loop = 0; loop < LOOPS; loop++) {
		int n1 = 0, n2 = 12, n3 = 14, n4 = 345, n6 = 210, n7 = 32, n8 = 899, n9 = 616, n10 = 0, n11 = 93;
		x1 = 1.0; x2 = -1.0; x3 = -1.0; x4 = -1.0;
		for (int i = 0; i < n1; i++) {
			x1 = (x1 + x2 + x3 - x4) * t;
			x2 = (x1 + x2 - x3 + x4) * t;
			x3 = (x1 - x2 + x3 + x4) * t;
			x4 = (-x1 + x2 + x3 + x4) * t;
		}
		e1[0] = 1.0; e1[1] = -1.0; e1[2] = -1.0; e1[3] = -1.0;
		for (int i = 0; i < n2; i++) {
			e1[0] = (e1[0] + e1[1] + e1[2] - e1[3]) * t;
			e1[1] = (e1[0] + e1[1] - e1[2] + e1[3]) * t;
			e1[2] = (e1[0] - e1[1] + e1[2] + e1[3]) * t;
			e1[3] = (-e1[0] + e1[1] + e1[2] + e1[3]) * t;
		}
		for (int i = 0; i < n3; i++)
			pa(e1);
		j = 1;
		for (int i = 0; i < n4; i++) {
			j = j == 1 ? 2 : 3;
			j = j > 2 ? 0 : 1;
			j = j < 1 ? 1 : 0;
		}
		j = 1; k = 2; l = 3;
		for (int i = 0; i < n6; i++) {
			j = j * (k - j) * (l - k);
			k = l * k - (l - j) * k;
			l = (l - k) * (k + j);
			e1[l - 2] = j + k + l;
			e1[k - 2] = j * k * l;
		}
		x = 0.5; y = 0.5;
		for (int i = 0; i < n7; i++) {
			x = t * __builtin_atan(t2 * __builtin_sin(x) * __builtin_cos(x) /
					       (__builtin_cos(x + y) + __builtin_cos(x - y) - 1.0));
			y = t * __builtin_atan(t2 * __builtin_sin(y) * __builtin_cos(y) /
					       (__builtin_cos(x + y) + __builtin_cos(x - y) - 1.0));
		}
		x = 1.0; y = 1.0; z = 1.0;
		for (int i = 0; i < n8; i++)
			p3(x, y, &z);
		j = 0; k = 1; l = 2;
		e1[0] = 1.0; e1[1] = 2.0; e1[2] = 3.0;
		for (int i = 0; i < n9; i++)
			p0();
		j = 2; k = 3;
		for (int i = 0; i < n10; i++) {
			j = j + k; k = j + k; j = k - j; k = k - j - j;
		}
		x = 0.75;
		for (int i = 0; i < n11; i++)
			x = __builtin_sqrt(__builtin_exp(__builtin_log(x) / t1));
		sum += x1 + x2 + x3 + x4 + e1[0] + e1[1] + e1[2] + e1[3] + x + y + z + j + k + l;
	}
	(void)t1;
	return (unsigned)(long long)h(sum);
}
