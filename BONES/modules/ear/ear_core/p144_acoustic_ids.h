#ifndef P144_ACOUSTIC_IDS_H
#define P144_ACOUSTIC_IDS_H

/* Neutral cross-team DATA contract. Neither Eye nor Ear includes the other's
   headers; Bones owns the contact and chooses one category for that surface. */
#define P144_ACOUSTIC_VERSION 1u

typedef enum {
    P144_ACOUSTIC_MINERAL = 0,
    P144_ACOUSTIC_WOOD = 1,
    P144_ACOUSTIC_METAL = 2,
    P144_ACOUSTIC_EARTH = 3,
    P144_ACOUSTIC_VEGETATION = 4,
    P144_ACOUSTIC_FABRIC = 5,
    P144_ACOUSTIC_COUNT = 6
} P144AcousticCategory;

#endif
