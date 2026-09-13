static const int g_yolo_anchor[YOLO_HEADS][YOLO_ANCHORS * 2] = {
	{  10,  13,  16,  30,  33,  23 },   /* stride 8  */
	{  30,  61,  62,  45,  59, 119 },   /* stride 16 */
	{ 116,  90, 156, 198, 373, 326 },   /* stride 32 */
};
static const int g_yolo_stride[YOLO_HEADS] = { 8, 16, 32 };
