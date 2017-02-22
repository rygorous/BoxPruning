///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 *	Contains code for the "box pruning revisited" project.
 *	\file		IceBoxPruning.cpp
 *	\author		Pierre Terdiman
 *	\date		February 2017
 */
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Precompiled Header
#include "Stdafx.h"

using namespace Meshmerizer;

// InsertionSort has better coherence, RadixSort is better for one-shot queries.
#define PRUNING_SORTER	RadixSort
//#define PRUNING_SORTER	InsertionSort

static __forceinline int intersects2D(const AABB& a, const AABB& b)
{
       if(    b.mMax.y < a.mMin.y || a.mMax.y < b.mMin.y
       ||     b.mMax.z < a.mMin.z || a.mMax.z < b.mMin.z)
              return 0;
       return 1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 *	Bipartite box pruning. Returns a list of overlapping pairs of boxes, each box of the pair belongs to a different set.
 *	\param		nb0		[in] number of boxes in the first set
 *	\param		list0	[in] list of boxes for the first set
 *	\param		nb1		[in] number of boxes in the second set
 *	\param		list1	[in] list of boxes for the second set
 *	\param		pairs	[out] list of overlapping pairs
 *	\param		axes	[in] projection order (0,2,1 is often best)
 *	\return		true if success.
 */
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Meshmerizer::BipartiteBoxPruning(udword nb0, const AABB* list0, udword nb1, const AABB* list1, Container& pairs)
{
	// Checkings
	if(!nb0 || !list0 || !nb1 || !list1)
		return false;

	AABB* BoxList0 = new AABB[nb0+1];
	AABB* BoxList1 = new AABB[nb1+1];
	udword* Remap0;
	udword* Remap1;
	{
		// Allocate some temporary data
		float* PosList0 = new float[nb0+1];
		float* PosList1 = new float[nb1+1];

		// 1) Build main lists using the primary axis
		for(udword i=0;i<nb0;i++)
			PosList0[i] = list0[i].mMin.x;
		PosList0[nb0] = FLT_MAX;
		for(udword i=0;i<nb1;i++)
			PosList1[i] = list1[i].mMin.x;
		PosList1[nb1] = FLT_MAX;

		// 2) Sort the lists
		static PRUNING_SORTER RS0, RS1;	// Static for coherence.
		Remap0 = RS0.Sort(PosList0, nb0+1).GetRanks();
		Remap1 = RS1.Sort(PosList1, nb1+1).GetRanks();

		for(udword i=0;i<nb0;i++)
			BoxList0[i] = list0[Remap0[i]];
		BoxList0[nb0].mMin.x = FLT_MAX;

		for(udword i=0;i<nb1;i++)
			BoxList1[i] = list1[Remap1[i]];
		BoxList1[nb1].mMin.x = FLT_MAX;

		DELETEARRAY(PosList1);
		DELETEARRAY(PosList0);
	}

	// 3) Prune the lists
	udword Index0 = 0;
	udword RunningAddress1 = 0;
	while(RunningAddress1<nb1 && Index0<nb0)
	{
		const AABB& Box0 = BoxList0[Index0];

		const float MinLimit = Box0.mMin.x;
		while(BoxList1[RunningAddress1].mMin.x<MinLimit)
			RunningAddress1++;

		const udword RIndex0 = Remap0[Index0];
		udword Index1 = RunningAddress1;
		const float MaxLimit = Box0.mMax.x;
		while(BoxList1[Index1].mMin.x<=MaxLimit)
		{
			if(intersects2D(Box0, BoxList1[Index1]))
				pairs.Add(RIndex0).Add(Remap1[Index1]);

			Index1++;
		}
		Index0++;
	}

	////

	Index0 = 0;
	udword RunningAddress0 = 0;
	while(RunningAddress0<nb0 && Index0<nb1)
	{
		const AABB& Box1 = BoxList1[Index0];

		const float MinLimit = Box1.mMin.x;
		while(BoxList0[RunningAddress0].mMin.x<=MinLimit)
			RunningAddress0++;

		const udword RIndex1 = Remap1[Index0];
		udword Index1 = RunningAddress0;
		const float MaxLimit = Box1.mMax.x;
		while(BoxList0[Index1].mMin.x<=MaxLimit)
		{
			if(intersects2D(BoxList0[Index1], Box1))
				pairs.Add(Remap0[Index1]).Add(RIndex1);

			Index1++;
		}
		Index0++;
	}

	DELETEARRAY(BoxList1);
	DELETEARRAY(BoxList0);

	return true;
}


// Munge the float bits to return produce an unsigned order-preserving
// ranking of floating-point numbers.
// (Old trick: http://stereopsis.com/radix.html FloatFlip, with a new
// spin to get rid of -0.0f)

// In /fp:precise, we can just calc "x + 0.0f" and get what we need.
// But fast math optimizes it away. Could use #pragma float_control,
// but that prohibits inlining of MungeFloat. So do this silly thing
// instead.
float g_global_this_always_zero = 0.0f;

union FloatOrInt32
{
	float f;
	sdword s;
};

static inline udword MungeFloat(float f)
{
	FloatOrInt32 u;
    u.f = f + g_global_this_always_zero;  // NOT a nop! Canonicalizes -0.0f to +0.0f
    udword toggle = (u.s >> 31) & ~(1u << 31);
    return u.s ^ toggle;
}

	#include <xmmintrin.h>
	#include <emmintrin.h>

static inline __m128i MungeFloatSSE(__m128 f)
{
	f = _mm_add_ps(f, _mm_setzero_ps()); // adding 0 canonicalizes -0.0f to +0.0f
	__m128i sign = _mm_srai_epi32(_mm_castps_si128(f), 31);
	__m128i toggle = _mm_and_si128(sign, _mm_set1_epi32(0x7fffffff));
	return _mm_xor_si128(_mm_castps_si128(f), toggle);
}

// Pair output buffer. We use this instead of a Container because we want slightly different
// insertion semantics. No real abstraction in here; seeing as the whole point of this is to
// (eventually) poke around in these fields from ASM code, it seems pointless.
//
// While the PairOutputBuffer is active, it takes over management of the storage for the
// underlying Container. On destruction, it returns the storage back to the container.
struct PairOutputBuffer
{
	static const size_t kSlack = 16; // distance from the high watermark to the actual capacity

	udword*	mEnd;			// Pointer to current end (just past last inserted element)
	udword* mHighWatermark;	// Pointer to kSlack elements before the end of the allocated storage
	udword* mBegin;			// Pointer to beginning of storage
	Container &mHost;		// The container we're outputting to.

	PairOutputBuffer(Container &host);
	~PairOutputBuffer();
};

PairOutputBuffer::PairOutputBuffer(Container &host)
	: mHost(host)
{
	if (mHost.GetCapacity() < kSlack)
		mHost.Resize(kSlack);

	mBegin = host.GetEntries() + host.GetNbEntries();
	mEnd = mBegin + host.GetNbEntries();
	mHighWatermark = mBegin + host.GetCapacity() - kSlack;
}

PairOutputBuffer::~PairOutputBuffer()
{
	// Return storage back to the container.
	mHost.mEntries = mBegin;
	mHost.mCurNbEntries = mEnd - mBegin;
	mHost.mMaxNbEntries = (mHighWatermark + kSlack) - mBegin;
}

static __declspec(noinline) void GrowPairOutputBuffer(PairOutputBuffer &buf)
{
	size_t numEntries = buf.mEnd - buf.mBegin;
	size_t newCapacity = numEntries * 2 + 2*PairOutputBuffer::kSlack;

	udword* NewEntries = new udword[newCapacity];
	CopyMemory(NewEntries, buf.mBegin, numEntries*sizeof(udword));
	DELETEARRAY(buf.mBegin);

	buf.mBegin = NewEntries;
	buf.mEnd = buf.mBegin + numEntries;
	buf.mHighWatermark = buf.mBegin + newCapacity - PairOutputBuffer::kSlack;
}

// Count trailing zeroes
static inline udword Ctz32(udword x)
{
	unsigned long idx;
	_BitScanForward(&idx, x);
	return idx;
}

// Reports a bunch of intersections as specified by a base index and a bit mask.
static void __cdecl ReportIntersections(PairOutputBuffer& POB, udword remap_id0, const udword* remap_base, udword mask)
{
	// Make sure there's enough space to insert our new elements
	if (POB.mEnd > POB.mHighWatermark)
		GrowPairOutputBuffer(POB);

	udword *Pairs = POB.mEnd;

	do
	{
		*Pairs++ = remap_id0;
		*Pairs++ = remap_base[Ctz32(mask)];
		mask &= mask - 1;
	} while (mask);

	POB.mEnd = Pairs;
}

static void Error()
{
	printf("ERROR!\n");
}

template<typename T>
static inline T* PtrAddBytes(T* ptr, ptrdiff_t bytes)
{
	return (T*)((char*)ptr + bytes);
}

static void BoxPruningKernelIntrinsics(PairOutputBuffer &POB, FloatOrInt32* BoxBase, FloatOrInt32* BoxEnd, udword* Remap, ptrdiff_t BoxBytesP)
{
	ptrdiff_t BoxBytesN = -BoxBytesP;
	FloatOrInt32 *Box0Ptr = BoxBase; // corresponds to Index0
	FloatOrInt32 *RunningPtr = BoxBase; // corresponds to RunningAddress
	while(Box0Ptr < BoxEnd)
	{
		const sdword MinLimit = PtrAddBytes(Box0Ptr, 2*BoxBytesN)->s;
		while (PtrAddBytes(RunningPtr++, 2*BoxBytesN)->s < MinLimit);
		if (RunningPtr >= BoxEnd)
			break;

		const FloatOrInt32 *MaxLimitPtr = PtrAddBytes(PtrAddBytes(Box0Ptr, 2*BoxBytesN), BoxBytesN);
		const sdword MaxLimit = MaxLimitPtr->s;
		udword RIndex0 = Remap[Box0Ptr - BoxBase];

		__m128i MaxLimitVec = _mm_set1_epi32(MaxLimitPtr->s);
		__m128 Box0MaxY = _mm_set1_ps(PtrAddBytes(Box0Ptr, 1*BoxBytesN)->f);
		__m128 Box0MinY = _mm_set1_ps(PtrAddBytes(Box0Ptr, 0*BoxBytesP)->f);
		__m128 Box0MaxZ = _mm_set1_ps(PtrAddBytes(Box0Ptr, 1*BoxBytesP)->f);
		__m128 Box0MinZ = _mm_set1_ps(PtrAddBytes(Box0Ptr, 2*BoxBytesP)->f);

		// Main loop
		const FloatOrInt32 *Box1Ptr = RunningPtr;
		while (PtrAddBytes(Box1Ptr + 3, 2*BoxBytesN)->s <= MaxLimit) // while Box[Index1+3].mMinX <= MaxLimit
		{
			// Load 4 boxes worth
			__m128 Box1MaxY = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 1*BoxBytesN)->f);
			__m128 Box1MinY = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 0*BoxBytesP)->f);
			__m128 Box1MaxZ = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 1*BoxBytesP)->f);
			__m128 Box1MinZ = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 2*BoxBytesP)->f);
			Box1Ptr += 4;

			// Intersection test:
			//  !(b.MaxY < a.MinY) && (b.MinY <= a.MaxY) && !(b.MaxZ < a.MinZ) && (b.MinZ <= a.MaxZ)
			__m128 Cmp;
			Cmp = _mm_cmpnlt_ps(Box1MaxY, Box0MinY);
			Cmp = _mm_and_ps(Cmp, _mm_cmple_ps(Box1MinY, Box0MaxY));
			Cmp = _mm_and_ps(Cmp, _mm_cmpnlt_ps(Box1MaxZ, Box0MinZ));
			Cmp = _mm_and_ps(Cmp, _mm_cmple_ps(Box1MinZ, Box0MaxZ));

			int Mask = _mm_movemask_ps(Cmp);
			if (Mask)
				ReportIntersections(POB, RIndex0, Remap + ((Box1Ptr - 4) - BoxBase), Mask);
		}

		// Tail group: first box is in, but one or more boxes with mMinX past MaxLimit inside.
		if (PtrAddBytes(Box1Ptr, 2*BoxBytesN)->s <= MaxLimit)
		{
			__m128i Box1MinX = _mm_loadu_si128((const __m128i *)&PtrAddBytes(Box1Ptr, 2*BoxBytesN)->s);
			__m128 OutsideMask = _mm_castsi128_ps(_mm_cmpgt_epi32(Box1MinX, MaxLimitVec));

			// Load 4 boxes worth
			__m128 Box1MaxY = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 1*BoxBytesN)->f);
			__m128 Box1MinY = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 0*BoxBytesP)->f);
			__m128 Box1MaxZ = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 1*BoxBytesP)->f);
			__m128 Box1MinZ = _mm_loadu_ps(&PtrAddBytes(Box1Ptr, 2*BoxBytesP)->f);

			// Intersection test:
			//  !(b.MaxY < a.MinY) && (b.MinY <= a.MaxY) && !(b.MaxZ < a.MinZ) && (b.MinZ <= a.MaxZ)
			__m128 Cmp;
			Cmp = _mm_andnot_ps(OutsideMask, _mm_cmpnlt_ps(Box1MaxY, Box0MinY));
			Cmp = _mm_and_ps(Cmp, _mm_cmple_ps(Box1MinY, Box0MaxY));
			Cmp = _mm_and_ps(Cmp, _mm_cmpnlt_ps(Box1MaxZ, Box0MinZ));
			Cmp = _mm_and_ps(Cmp, _mm_cmple_ps(Box1MinZ, Box0MaxZ));

			int Mask = _mm_movemask_ps(Cmp);
			if (Mask)
				ReportIntersections(POB, RIndex0, Remap + (Box1Ptr - BoxBase), Mask);
		}
		Box0Ptr++;
	}
}

static void BoxPruningKernelSSE2(PairOutputBuffer &POB, FloatOrInt32* BoxBase, FloatOrInt32* BoxEnd, udword* Remap, ptrdiff_t BoxBytesP)
{
	FloatOrInt32 *RunningPtr = BoxBase;
	udword BoxToRemap;

	__asm
	{
		mov			esi, [BoxBase];		// Box0Ptr
		xor			edx, edx;
		mov			ecx, [BoxBytesP];	// ecx = BoxBytesP
		sub			edx, ecx;			// edx = BoxBytesN
		mov			eax, [Remap];
		sub			eax, esi;     		// BoxToRemap = Remap - BoxBase
		mov			[BoxToRemap], eax;

OuterLoop:
		mov			edi, [RunningPtr];	// edi = Box1Ptr = RunningPtr
		mov			eax, [esi + 2*edx];	// eax = MinLimit

AdvanceRunningPtr:
		cmp			eax, [edi + 2*edx];
		lea			edi, [edi + 4];		// RunningPtr++
		jg			AdvanceRunningPtr;

		cmp			edi, [BoxEnd];		// RunningPtr >= BoxEnd?
		jae			AllDone;

		mov			[RunningPtr], edi;
		lea			ebx, [esi + 2*edx];
		mov			ebx, [ebx + edx];	// MaxLimit


ReloadBeforeMainLoop:
		movss		xmm4, [esi + edx];	// xmm4 = Box0MaxY
		shufps		xmm4, xmm4, 0;
		movss		xmm5, [esi];		// xmm5 = Box0MinY
		shufps		xmm5, xmm5, 0;
		movss		xmm6, [esi + ecx];	// xmm6 = Box0MaxZ
		shufps		xmm6, xmm6, 0;
		movss		xmm7, [esi + 2*ecx];// xmm7 = Box0MinZ
		shufps		xmm7, xmm7, 0;

		align		16
MainLoop:
		cmp			ebx, [edi + 2*edx + 12];	// Box[Index1+3].mMinX <= MaxLimit?
		jl			ProcessTail;

		movups		xmm0, [edi + edx];
		cmpnltps	xmm0, xmm5;			// Box1MaxY >= Box0MinY?

		movups		xmm1, [edi];
		cmpleps		xmm1, xmm4;			// Box1MinY <= Box0MaxY?
		andps		xmm0, xmm1;

		movups		xmm1, [edi + ecx];
		cmpnltps	xmm1, xmm7;			// Box1MaxZ >= Box0MinZ?
		andps		xmm0, xmm1;

		movups		xmm1, [edi + 2*ecx];
		cmpleps		xmm1, xmm6;			// Box1MinZ <= Box0MaxY?
		andps		xmm0, xmm1;

		add			edi, 16;			// Box1Ptr += 4
		movmskps	eax, xmm0;
		test		eax, eax;
		jz			MainLoop;

		push		ecx;
		push		edx;

			push		eax;			// "mask" arg for ReportIntersections
			lea			eax, [edi - 16];
			add			eax, [BoxToRemap]; // &Remap[Box1Ptr - 4 - BoxBase]
			push		eax;			// "remap_base" arg
			mov			eax, [BoxToRemap];
			push		dword ptr [eax + esi]; // "remap_id0" arg
			push		[POB];			// "POB" arg
			call		ReportIntersections;
			add			esp, 16;

		pop			edx;
		pop			ecx;
		jmp			ReloadBeforeMainLoop;

		align		16
ProcessTail:
		cmp			ebx, [edi + 2*edx];	// Box[Index1].mMinX <= MaxLimit?
		jl			LoopFooter;

		movd		xmm1, ebx;			// MaxLimit
		pshufd		xmm1, xmm1, 0;		// broadcast it!

		movdqu		xmm0, [edi + 2*edx];// Box1MinX
		pcmpgtd		xmm0, xmm1;			// OutsideMask

		movups		xmm1, [edi + edx];
		cmpnltps	xmm1, xmm5;			// Box1MaxY >= Box0MinY?
		andnps		xmm0, xmm1;

		movups		xmm1, [edi];
		cmpleps		xmm1, xmm4;			// Box1MinY <= Box0MaxY?
		andps		xmm0, xmm1;

		movups		xmm1, [edi + ecx];
		cmpnltps	xmm1, xmm7;			// Box1MaxZ >= Box0MinZ?
		andps		xmm0, xmm1;

		movups		xmm1, [edi + 2*ecx];
		cmpleps		xmm1, xmm6;			// Box1MinZ <= Box0MaxY?
		andps		xmm0, xmm1;

		movmskps	eax, xmm0;
		test		eax, eax;
		jz			LoopFooter;

		push		ecx;
		push		edx;

			push		eax;			// "mask" arg for ReportIntersections
			mov			eax, edi;
			add			eax, [BoxToRemap]; // &Remap[Box1Ptr - BoxBase]
			push		eax;			// "remap_base" arg
			mov			eax, [BoxToRemap];
			push		dword ptr [eax + esi]; // "remap_id0" arg
			push		[POB];			// "POB" arg
			call		ReportIntersections;
			add			esp, 16;

		pop			edx;
		pop			ecx;

LoopFooter:
		add			esi, 4;				// Box0Ptr++
		cmp			esi, [BoxEnd];		// Box0Ptr < BoxEnd?
		jb			OuterLoop;

AllDone:
	}
}

static void BoxPruningKernelAVX(PairOutputBuffer &POB, FloatOrInt32* BoxBase, FloatOrInt32* BoxEnd, udword* Remap, ptrdiff_t BoxBytesP)
{
	static const udword PreAlignMasks[16] = {
		 0u,  0u,  0u,  0u,  0u,  0u,  0u,  0u,
		~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
	};
	FloatOrInt32 *RunningPtr = BoxBase;
	udword BoxToRemap;

	__asm
	{
		vzeroupper;

		mov				esi, [BoxBase];		// Box0Ptr
		xor				edx, edx;
		mov				ecx, [BoxBytesP];	// ecx = BoxBytesP
		sub				edx, ecx;			// edx = BoxBytesN
		mov				eax, [Remap];
		sub				eax, esi;     		// BoxToRemap = Remap - BoxBase
		mov				[BoxToRemap], eax;

OuterLoop:
		mov				edi, [RunningPtr];	// edi = Box1Ptr = RunningPtr
		mov				eax, [esi + 2*edx];	// eax = MinLimit

AdvanceRunningPtr:
		cmp				eax, [edi + 2*edx];
		lea				edi, [edi + 4];		// RunningPtr++
		jg				AdvanceRunningPtr;

		cmp				edi, [BoxEnd];		// RunningPtr >= BoxEnd?
		jae				AllDone;

		mov				[RunningPtr], edi;
		lea				ebx, [esi + 2*edx];
		mov				ebx, [ebx + edx];	// MaxLimit

		// First iter tries to get us to alignment
		vbroadcastss	ymm4, [esi + edx];	// ymm4 = Box0MaxY
		vbroadcastss	ymm5, [esi];		// ymm5 = Box0MinY
		vbroadcastss	ymm6, [esi + ecx];	// ymm6 = Box0MaxZ
		vbroadcastss	ymm7, [esi + 2*ecx]; // ymm7 = Box0MinZ

		// Don't even bother trying if there's only a handful candidates for this box
		cmp				ebx, [edi + 2*edx + 28];	// Box[Index1+7].mMinX <= MaxLimit?
		jl				ProcessTail;

		mov				eax, edi;
		and				eax, 31;			// address mod 32
		neg				eax;
		vmovups			ymm0, [PreAlignMasks + 32 + eax];

		and				edi, not 31;		// ka-chunk!

		vcmpleps		ymm1, ymm5, [edi + edx];	// Box1MaxY >= Box0MinY?
		vandps 			ymm0, ymm0, ymm1;
		vcmpgeps		ymm1, ymm4, [edi];			// Box1MinY <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;
		vcmpleps		ymm1, ymm7, [edi + ecx];	// Box1MaxZ >= Box0MinZ?
		vandps			ymm0, ymm0, ymm1;
		vcmpgeps		ymm1, ymm6, [edi + 2*ecx];	// Box1MinZ <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;

		add				edi, 32;
		vmovmskps		eax, ymm0;
		test			eax, eax;
		jz				MainLoop;

		push			ecx;
		push			edx;
		vzeroupper;

			push		eax;			// "mask" arg for ReportIntersections
			lea			eax, [edi - 32];
			add			eax, [BoxToRemap]; // &Remap[Box1Ptr - 8 - BoxBase]
			push		eax;			// "remap_base" arg
			mov			eax, [BoxToRemap];
			push		dword ptr [eax + esi]; // "remap_id0" arg
			push		[POB];			// "POB" arg
			call		ReportIntersections;
			add			esp, 16;

		pop				edx;
		pop				ecx;
		jmp				ReloadBeforeMainLoop;

ReloadBeforeMainLoop:
		vbroadcastss	ymm4, [esi + edx];	// ymm4 = Box0MaxY
		vbroadcastss	ymm5, [esi];		// ymm5 = Box0MinY
		vbroadcastss	ymm6, [esi + ecx];	// ymm6 = Box0MaxZ
		vbroadcastss	ymm7, [esi + 2*ecx]; // ymm7 = Box0MinZ

		align			16
MainLoop:
		cmp				ebx, [edi + 2*edx + 28];	// Box[Index1+7].mMinX <= MaxLimit?
		jl				ProcessTail;

		vcmpleps		ymm0, ymm5, [edi + edx];	// Box1MaxY >= Box0MinY?
		vcmpgeps		ymm1, ymm4, [edi];			// Box1MinY <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;
		vcmpleps		ymm1, ymm7, [edi + ecx];	// Box1MaxZ >= Box0MinZ?
		vandps			ymm0, ymm0, ymm1;
		vcmpgeps		ymm1, ymm6, [edi + 2*ecx];	// Box1MinZ <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;

		add				edi, 32;			// Box1Ptr += 8
		vmovmskps		eax, ymm0;
		test			eax, eax;
		jz				MainLoop;

		push			ecx;
		push			edx;
		vzeroupper;

			push		eax;			// "mask" arg for ReportIntersections
			lea			eax, [edi - 32];
			add			eax, [BoxToRemap]; // &Remap[Box1Ptr - 8 - BoxBase]
			push		eax;			// "remap_base" arg
			mov			eax, [BoxToRemap];
			push		dword ptr [eax + esi]; // "remap_id0" arg
			push		[POB];			// "POB" arg
			call		ReportIntersections;
			add			esp, 16;

		pop				edx;
		pop				ecx;
		jmp				ReloadBeforeMainLoop;

		align			16
ProcessTail:
		cmp				ebx, [edi + 2*edx];	// Box[Index1].mMinX <= MaxLimit?
		jl				LoopFooter;

		vmovd			xmm2, ebx;			// MaxLimit
		vpshufd			xmm2, xmm2, 0;		// broadcast it!

		vmovdqu			xmm0, [edi + 2*edx];		// Box1MinX
		vmovdqu			xmm1, [edi + 2*edx + 16];
		vpcmpgtd		xmm0, xmm0, xmm2;			// OutsideMask
		vpcmpgtd		xmm1, xmm1, xmm2;
		vinsertf128		ymm0, ymm0, xmm1, 1; // Form 256-bit OutsideMask

		vcmpleps		ymm1, ymm5, [edi + edx];	// Box1MaxY >= Box0MinY?
		vandnps			ymm0, ymm0, ymm1;
		vcmpgeps		ymm1, ymm4, [edi];			// Box1MinY <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;
		vcmpleps		ymm1, ymm7, [edi + ecx];	// Box1MaxZ >= Box0MinZ?
		vandps			ymm0, ymm0, ymm1;
		vcmpgeps		ymm1, ymm6, [edi + 2*ecx];	// Box1MinZ <= Box0MaxY?
		vandps			ymm0, ymm0, ymm1;

		vmovmskps		eax, ymm0;
		test			eax, eax;
		jz				LoopFooter;

		push			ecx;
		push			edx;
		vzeroupper;

			push		eax;			// "mask" arg for ReportIntersections
			mov			eax, edi;
			add			eax, [BoxToRemap]; // &Remap[Box1Ptr - BoxBase]
			push		eax;			// "remap_base" arg
			mov			eax, [BoxToRemap];
			push		dword ptr [eax + esi]; // "remap_id0" arg
			push		[POB];			// "POB" arg
			call		ReportIntersections;
			add			esp, 16;

		pop				edx;
		pop				ecx;

LoopFooter:
		add				esi, 4;				// Box0Ptr++
		cmp				esi, [BoxEnd];		// Box0Ptr < BoxEnd?
		jb				OuterLoop;

AllDone:
		vzeroupper;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 *	Complete box pruning. Returns a list of overlapping pairs of boxes, each box of the pair belongs to the same set.
 *	\param		nb		[in] number of boxes
 *	\param		list	[in] list of boxes
 *	\param		pairs	[out] list of overlapping pairs
 *	\param		axes	[in] projection order (0,2,1 is often best)
 *	\return		true if success.
 */
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Meshmerizer::CompleteBoxPruning(udword nb, const AABB* list, Container& pairs)
{
	// Checkings
	if(!nb || !list)
		return false;

	udword nbpad = (nb+15) & ~7; // Align up to multiple of 8, and add an extra 8 of padding.
	ptrdiff_t BoxBytesP = nbpad*sizeof(FloatOrInt32);
	ptrdiff_t BoxBytesN = -BoxBytesP;
	ptrdiff_t BoxBytes3N = 3*BoxBytesN;

	// Our pair output buffer
	PairOutputBuffer POB(pairs);

	// BoxSOA: in order, arrays for MaxX,MinX (int), MaxY,MinY,MaxZ,MinZ (float).
	FloatOrInt32* BoxSOA = (FloatOrInt32*)_aligned_malloc(BoxBytesP * 6, 32);

	// Our default origin is actually pointing at array number 3 (MinY).
	FloatOrInt32* BoxBase = PtrAddBytes(BoxSOA, 3*BoxBytesP);
	FloatOrInt32* BoxEnd = BoxBase + nb;

	udword* Remap;
	{
		// Allocate some temporary data
		float* PosList = new float[nb+1];

		// 1) Build main list using the primary axis
		for(udword i=0;i<nb;i++)
			PosList[i] = list[i].mMin.x;
		PosList[nb] = FLT_MAX;

		// 2) Sort the list
		static PRUNING_SORTER RS;	// Static for coherence
		Remap = RS.Sort(PosList, nb+1).GetRanks();

		// 3) Prepare the SoA box array
		udword i;
		for(i=0;i<(nb & ~3);i += 4)
		{
			const AABB& Box0 = list[Remap[i+0]];
			const AABB& Box1 = list[Remap[i+1]];
			const AABB& Box2 = list[Remap[i+2]];
			const AABB& Box3 = list[Remap[i+3]];
			FloatOrInt32 *OutBoxI = &BoxBase[i];
			__m128 r0,r1,r2,r3;
			__m128i i0,i1;

			r0 = _mm_loadu_ps(&Box0.mMin.x);
			r1 = _mm_loadu_ps(&Box1.mMin.x);
			r2 = _mm_loadu_ps(&Box2.mMin.x);
			r3 = _mm_loadu_ps(&Box3.mMin.x);
			_MM_TRANSPOSE4_PS(r0,r1,r2,r3); // r0 = MinX, r1 = MinY, r2 = MinZ, r3 = MaxX

			i0 = MungeFloatSSE(r0); // munged MinX
			i1 = MungeFloatSSE(r3); // munged MaxX
			_mm_store_si128((__m128i *) &PtrAddBytes(OutBoxI, 2*BoxBytesN)->s, i0); // MinX
			_mm_store_si128((__m128i *) &PtrAddBytes(OutBoxI,  BoxBytes3N)->s, i1); // MaxX
			_mm_store_ps(&PtrAddBytes(OutBoxI, 0*BoxBytesP)->f, r1); // MinY
			_mm_store_ps(&PtrAddBytes(OutBoxI, 2*BoxBytesP)->f, r2); // MinZ

			r0 = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&Box0.mMax.y));
			r1 = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&Box1.mMax.y));
			r2 = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&Box2.mMax.y));
			r3 = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&Box3.mMax.y));
			_MM_TRANSPOSE4_PS(r0,r1,r2,r3); // r0 = MaxY, r1=MaxZ
			_mm_store_ps(&PtrAddBytes(OutBoxI, 1*BoxBytesN)->f, r0); // MaxY
			_mm_store_ps(&PtrAddBytes(OutBoxI, 1*BoxBytesP)->f, r1); // MaxZ
		}
		for(;i<nb;i++)
		{
			const AABB& Box = list[Remap[i]];
			FloatOrInt32 *OutBoxI = &BoxBase[i];
			PtrAddBytes(OutBoxI,  BoxBytes3N)->s = MungeFloat(Box.mMax.x);
			PtrAddBytes(OutBoxI, 2*BoxBytesN)->s = MungeFloat(Box.mMin.x);
			PtrAddBytes(OutBoxI, 1*BoxBytesN)->f = Box.mMax.y;
			PtrAddBytes(OutBoxI, 0*BoxBytesP)->f = Box.mMin.y;
			PtrAddBytes(OutBoxI, 1*BoxBytesP)->f = Box.mMax.z;
			PtrAddBytes(OutBoxI, 2*BoxBytesP)->f = Box.mMin.z;
		}
		for(;i<nbpad;i++)
		{
			FloatOrInt32 *OutBoxI = &BoxBase[i];
			PtrAddBytes(OutBoxI,  BoxBytes3N)->s = -0x80000000;
			PtrAddBytes(OutBoxI, 2*BoxBytesN)->s = 0x7fffffff;
			PtrAddBytes(OutBoxI, 1*BoxBytesN)->f = -FLT_MAX;
			PtrAddBytes(OutBoxI, 0*BoxBytesP)->f = FLT_MAX;
			PtrAddBytes(OutBoxI, 1*BoxBytesP)->f = -FLT_MAX;
			PtrAddBytes(OutBoxI, 2*BoxBytesP)->f = FLT_MAX;
		}
		DELETEARRAY(PosList);
	}

	// 4) Prune the list
#if 0
	BoxPruningKernelIntrinsics(POB, BoxBase, BoxEnd, Remap, BoxBytesP);
#else
	int info[4];
	__cpuid(info, 1);

	// Use AVX if CPU and OS support it
	if ((info[2] & 0x18000000) == 0x18000000)
		BoxPruningKernelAVX(POB, BoxBase, BoxEnd, Remap, BoxBytesP);
	else
		BoxPruningKernelSSE2(POB, BoxBase, BoxEnd, Remap, BoxBytesP);
#endif

	_aligned_free(BoxSOA);
	return true;
}

