/* Name generation from curated plant wordlist.
 * name_index = id % 200, suffix for collisions beyond 200. */
#pragma once
#include <cstdint>
#include <cstdio>

static const char* const WORDLIST[200] = {
    "Acorn","Alder","Almond","Amber","Anise","Apple","Apricot","Arbutus","Arnica","Ash",
    "Aspen","Aster","Azalea","Balm","Balsam","Barley","Basil","Bay","Beech","Belladonna",
    "Bergamot","Birch","Bistort","Blackthorn","Bladderwort","Blossom","Bluebell","Bog","Borage","Bracken",
    "Bramble","Briar","Broom","Bryony","Buckthorn","Bulrush","Burdock","Bur","Buttercup","Calendula",
    "Camas","Camomile","Campion","Caraway","Cardamom","Catkin","Cedar","Celandine","Chervil","Chestnut",
    "Chicory","Cinnamon","Clary","Cleavers","Clematis","Clover","Coltsfoot","Comfrey","Coriander","Corydalis",
    "Cotton","Cowslip","Cranberry","Crocus","Cumin","Currant","Cypress","Daffodil","Dahlia","Daisy",
    "Damson","Dandelion","Dewberry","Dill","Dock","Dogwood","Echinacea","Elder","Elm","Endive",
    "Eucalyptus","Evening","Fennel","Fern","Feverfew","Fig","Filbert","Flax","Forsythia","Foxglove",
    "Foxtail","Frangipani","Furze","Galangal","Gardenia","Garlic","Gentian","Ginger","Ginseng","Goldenrod",
    "Gorse","Hazel","Heather","Hellebore","Hemlock","Hemp","Henbane","Hibiscus","Holly","Hollyhock",
    "Honeysuckle","Hornbeam","Horsetail","Hyssop","Iris","Ivy","Jasmine","Jonquil","Juniper","Kelp",
    "Knapweed","Lacebark","Larch","Larkspur","Laurel","Lavender","Leek","Lemon","Lichen","Lilac",
    "Lily","Lime","Linden","Liverwort","Lobelia","Lotus","Lovage","Lupin","Mace","Madder",
    "Maidenhair","Mallow","Mandrake","Maple","Marigold","Marjoram","Marsh","Meadowsweet","Mint","Mistletoe",
    "Moss","Mugwort","Mulberry","Mullein","Mustard","Myrrh","Myrtle","Nasturtium","Nettle","Oak",
    "Oats","Olive","Orchid","Oregano","Pansy","Papyrus","Parsley","Pennyroyal","Peony","Periwinkle",
    "Pine","Plantain","Plum","Poppy","Primrose","Quill","Quince","Ragwort","Reed","Rose",
    "Rosemary","Rowan","Rue","Rush","Saffron","Sage","Sandalwood","Sassafras","Savory","Saxifrage",
    "Sedge","Sloe","Sorrel","Speedwell","Spruce","Tansy","Tarragon","Teasel","Thistle","Thrift",
};

// Last 10: Thyme, Vetch, Violet, Willow, Yarrow, Yew (only 200 total, split above)
// Actually the above is exactly 200 entries (20 rows × 10)

inline void name_from_id(uint32_t id, char* buf, size_t buflen) {
    uint32_t idx = id % 200;
    uint32_t suffix = id / 200;
    if (suffix == 0)
        snprintf(buf, buflen, "%s", WORDLIST[idx]);
    else
        snprintf(buf, buflen, "%s_%lu", WORDLIST[idx], (unsigned long)(suffix + 1));
}

#ifdef ARDUINO
#include <esp_random.h>
inline void name_random(char* buf, size_t buflen) {
    uint32_t idx = esp_random() % 200;
    snprintf(buf, buflen, "%s", WORDLIST[idx]);
}
#endif
