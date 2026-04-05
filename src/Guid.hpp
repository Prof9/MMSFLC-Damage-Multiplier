#pragma once

#include <compare>
#include <cstdint>
#include <string>

#pragma pack(push, 1)
struct Guid
{
public:
	std::uint32_t data1;
	std::uint16_t data2;
	std::uint16_t data3;
	union
	{
		struct
		{
			std::uint8_t data4_0;
			std::uint8_t data4_1;
			std::uint8_t data4_2;
			std::uint8_t data4_3;
			std::uint8_t data4_4;
			std::uint8_t data4_5;
			std::uint8_t data4_6;
			std::uint8_t data4_7;
		};
		std::uint64_t data4L;
		struct
		{
			std::uint32_t data4L_0;
			std::uint32_t data4L_1;
		};
	};

	Guid(std::wstring const &str);

	std::wstring toString() const;

	std::strong_ordering compare(Guid const &other) const;

	bool operator==(Guid const &other) const { return this->compare(other) == 0; }
	std::strong_ordering operator<=>(Guid const &other) const { return this->compare(other); }
};
#pragma pack(pop)
