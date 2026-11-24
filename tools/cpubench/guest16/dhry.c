/*
 * dhry: Dhrystone 2.1 (Weicker), for cpubench's bench16: built with
 * i686-elf-gcc -m16, so 32-bit code under 66h/67h prefixes in real mode,
 * as CheckIt's own benchmark runs. Tiny model (CS = DS = SS = ES), entered
 * from dhry_start.S; the result's checksum is left in EAX before the HLT.
 */
#define LOOPS 2000

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int One_Thirty;
typedef int One_Fifty;
typedef char Capital_Letter;
typedef int Boolean;
typedef char Str_30[31];
typedef int Arr_1_Dim[50];
typedef int Arr_2_Dim[50][50];

typedef struct record {
	struct record *Ptr_Comp;
	Enumeration Discr;
	union {
		struct { Enumeration Enum_Comp; int Int_Comp; char Str_Comp[31]; } var_1;
		struct { Enumeration E_Comp_2; char Str_2_Comp[31]; } var_2;
		struct { char Ch_1_Comp; char Ch_2_Comp; } var_3;
	} variant;
} Rec_Type, *Rec_Pointer;

static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int Int_Glob;
static Boolean Bool_Glob;
static char Ch_1_Glob, Ch_2_Glob;
static Arr_1_Dim Arr_1_Glob;
static Arr_2_Dim Arr_2_Glob;
static Rec_Type rec_a, rec_b;

static void strcpy_(char *d, const char *s) { while ((*d++ = *s++)) ; }
static int strcmp_(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

static Boolean Func_3(Enumeration Enum_Par_Val) { return Enum_Par_Val == Ident_3; }

static Enumeration Func_1(Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
{
	Capital_Letter Ch_1_Loc = Ch_1_Par_Val, Ch_2_Loc = Ch_1_Loc;
	if (Ch_2_Loc != Ch_2_Par_Val)
		return Ident_1;
	Ch_1_Glob = Ch_1_Loc;
	return Ident_2;
}

static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
	One_Thirty Int_Loc = 2;
	Capital_Letter Ch_Loc = 'A';
	while (Int_Loc <= 2)
		if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
			Ch_Loc = 'A';
			Int_Loc += 1;
		}
	if (Ch_Loc >= 'W' && Ch_Loc < 'Z')
		Int_Loc = 7;
	if (Ch_Loc == 'R')
		return 1;
	if (strcmp_(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
		Int_Loc += 7;
		Int_Glob = Int_Loc;
		return 1;
	}
	return 0;
}

static void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val, One_Fifty *Int_Par_Ref)
{
	*Int_Par_Ref = Int_2_Par_Val + Int_1_Par_Val + 2;
}

static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref, int Int_1_Par_Val, int Int_2_Par_Val)
{
	One_Fifty Int_Loc = Int_1_Par_Val + 5, Int_Index;
	Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
	Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
	Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
	for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
		Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
	Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
	Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
	Int_Glob = 5;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
	*Enum_Ref_Par = Enum_Val_Par;
	if (!Func_3(Enum_Val_Par))
		*Enum_Ref_Par = Ident_4;
	switch (Enum_Val_Par) {
	case Ident_1: *Enum_Ref_Par = Ident_1; break;
	case Ident_2: *Enum_Ref_Par = Int_Glob > 100 ? Ident_1 : Ident_4; break;
	case Ident_3: *Enum_Ref_Par = Ident_2; break;
	case Ident_4: break;
	case Ident_5: *Enum_Ref_Par = Ident_3; break;
	}
}

static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
	if (Ptr_Glob != 0)
		*Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
	Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
	Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
	*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
	Ptr_Val_Par->variant.var_1.Int_Comp = 5;
	Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
	Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
	Proc_3(&Next_Record->Ptr_Comp);
	if (Next_Record->Discr == Ident_1) {
		Next_Record->variant.var_1.Int_Comp = 6;
		Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp, &Next_Record->variant.var_1.Enum_Comp);
		Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
		Proc_7(Next_Record->variant.var_1.Int_Comp, 10, &Next_Record->variant.var_1.Int_Comp);
	} else
		*Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
}

static void Proc_2(One_Fifty *Int_Par_Ref)
{
	One_Fifty Int_Loc = *Int_Par_Ref + 10;
	Enumeration Enum_Loc = Ident_2;
	for (;;) {
		if (Ch_1_Glob == 'A') {
			Int_Loc -= 1;
			*Int_Par_Ref = Int_Loc - Int_Glob;
			Enum_Loc = Ident_1;
		}
		if (Enum_Loc == Ident_1)
			break;
	}
}

static void Proc_4(void)
{
	Boolean Bool_Loc = Ch_1_Glob == 'A';
	Bool_Glob = Bool_Loc | Bool_Glob;
	Ch_2_Glob = 'B';
}

static void Proc_5(void)
{
	Ch_1_Glob = 'A';
	Bool_Glob = 0;
}

unsigned dhry_main(void)
{
	One_Fifty Int_1_Loc, Int_2_Loc, Int_3_Loc;
	char Ch_Index;
	Enumeration Enum_Loc;
	Str_30 Str_1_Loc, Str_2_Loc;
	int Run_Index;

	Next_Ptr_Glob = &rec_b;
	Ptr_Glob = &rec_a;
	Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
	Ptr_Glob->Discr = Ident_1;
	Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
	Ptr_Glob->variant.var_1.Int_Comp = 40;
	strcpy_(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING");
	strcpy_(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
	Arr_2_Glob[8][7] = 10;

	for (Run_Index = 1; Run_Index <= LOOPS; ++Run_Index) {
		Proc_5();
		Proc_4();
		Int_1_Loc = 2;
		Int_2_Loc = 3;
		strcpy_(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
		Enum_Loc = Ident_2;
		Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
		while (Int_1_Loc < Int_2_Loc) {
			Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
			Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
			Int_1_Loc += 1;
		}
		Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
		Proc_1(Ptr_Glob);
		for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index)
			if (Enum_Loc == Func_1(Ch_Index, 'C')) {
				Proc_6(Ident_1, &Enum_Loc);
				strcpy_(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
				Int_2_Loc = Run_Index;
				Int_Glob = Run_Index;
			}
		Int_2_Loc = Int_2_Loc * Int_1_Loc;
		Int_1_Loc = Int_2_Loc / Int_3_Loc;
		Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
		Proc_2(&Int_1_Loc);
	}
	/* (the standard end values, folded: a wrong instruction changes it) */
	return Int_Glob * 1000003u ^ Bool_Glob * 7919u ^ Ch_1_Glob ^ Ch_2_Glob << 8 ^ Arr_1_Glob[8] << 16 ^
	       Arr_2_Glob[8][7] * 31u ^ Ptr_Glob->variant.var_1.Int_Comp * 131u ^ Int_1_Loc * 17u ^ Int_2_Loc * 19u ^
	       Int_3_Loc * 23u ^ Enum_Loc * 29u ^ (unsigned)Next_Ptr_Glob->variant.var_1.Int_Comp * 37u;
}
