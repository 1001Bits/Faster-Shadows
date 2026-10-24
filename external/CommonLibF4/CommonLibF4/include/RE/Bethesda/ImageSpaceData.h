#pragma once

#include "RE/Bethesda/BSFixedString.h"
#include "RE/NetImmerse/NiSmartPointer.h"
#include "RE/NetImmerse/NiTexture.h"

namespace RE
{
	namespace BSGraphics
	{
		class Texture;
	}

	struct ImageSpaceBaseData
	{
	public:
		struct HDR  // HNAM
		{
		public:
			// members
			float eyeAdaptSpeed;          // 00
			float bloomBlurRadius;        // 04
			float bloomThreshold;         // 08
			float bloomScale;             // 0C
			float receiveBloomThreshold;  // 10
			float white;                  // 14
			float sunlightScale;          // 18
			float skyScale;               // 1C
			float eyeAdaptStrength;       // 20
		};
		static_assert(sizeof(HDR) == 0x24);

		struct Cinematic  // CNAM
		{
		public:
			// members
			float saturation;  // 0
			float brightness;  // 4
			float contrast;    // 8
		};
		static_assert(sizeof(Cinematic) == 0xC);

		struct Tint  // TNAM
		{
		public:
			struct ColorF
			{
			public:
				// members
				float red;    // 0
				float green;  // 4
				float blue;   // 8
			};
			static_assert(sizeof(ColorF) == 0xC);

			// members
			float  amount;  // 00
			ColorF color;   // 04
		};
		static_assert(sizeof(Tint) == 0x10);

		// members
		HDR       hdrData;        // 00
		Cinematic cinematicData;  // 24
		Tint      tintData;       // 30
		float     dofData[6];     // 40
	};
	static_assert(sizeof(ImageSpaceBaseData) == 0x58);

	struct ImageSpaceLUTData
	{
	public:
		// members
		BSFixedString        filename[4];   // 00
		float                weight[4];     // 20
		NiPointer<NiTexture> niTexture[4];  // 30
		BSGraphics::Texture* texture[4];    // 50
	};
	static_assert(sizeof(ImageSpaceLUTData) == 0x70);

	struct ImageSpaceModData
	{
	public:
		// members
		float data[20];  // 00
	};
	static_assert(sizeof(ImageSpaceModData) == 0x50);

	struct ImageSpaceData
	{
	public:
		// members
		ImageSpaceBaseData baseData;           //00
		ImageSpaceModData  modData;            //58
		float              highestTintAmount;  //A8
		float              highestFadeAmount;  //AC
	};
	static_assert(sizeof(ImageSpaceData) == 0xB0);
}
