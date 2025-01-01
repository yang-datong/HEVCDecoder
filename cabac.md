## 解码split_cu_flag

1. 找表：Table 9-48 – Assignment of ctxInc to syntax elements with context coded bins，中说明了binIdx只会为0,因为其他情况都是na，那么0列中为0,1,2(clause 9.3.4.2.2)，这表示计算出的 ctxInc 值可能为 0 或 1，而2实际上是指可能的 ctxInc 的数量。

2. 找到clause 9.3.4.2.2公式，计算出ctxInc。此节描述了使用相邻块（左侧和上方）的信息来计算 ctxInc 的通用过程，具体来说，根据 condL、availableL、condA 和 availableA 计算 split_cu_flag 的 ctxInc：

   1. **确定 (x0, y0)、(xNbL, yNbL) 和 (xNbA, yNbA)：**
      1. (x0, y0) 是当前块左上角亮度样本的位置。
      2. (xNbL, yNbL) = (x0 - 1, y0) 是左侧相邻块的位置。
      3. (xNbA, yNbA) = (x0, y0 - 1) 是上方相邻块的位置。

   **2. 确定 availableL 和 availableA：**

   - 你需要根据 HEVC 规范的 6.4.1 节中描述的可用性推导过程来确定 availableL 和 availableA。这通常涉及检查相邻块是否存在于当前切片/图块内，以及是否已经被解码。

   **3. 根据语法元素计算 condL 和 condA：**

   - **对于 split_cu_flag：**
     - condL = (CtDepth[ xNbL ][ yNbL ] > cqtDepth)：如果左侧相邻块的编码树深度大于当前编码四叉树深度，则 condL 为真。
     - condA = (CtDepth[ xNbA ][ yNbA ] > cqtDepth)：如果上方相邻块的编码树深度大于当前编码四叉树深度，则 condA 为真。
   - **对于 cu_skip_flag：**
     - condL = cu_skip_flag[ xNbL ][ yNbL ]：如果左侧相邻块的 cu_skip_flag 为 1，则 condL 为真。
     - condA = cu_skip_flag[ xNbA ][ yNbA ]：如果上方相邻块的 cu_skip_flag 为 1，则 condA 为真。

   **4. 计算 ctxInc：**

   - ctxInc = ( condL && availableL ) + ( condA && availableA )
   - 这意味着：
     - 如果 condL 和 availableL 都为真，则加 1。
     - 如果 condA 和 availableA 都为真，则加 1。
     - ctxInc 的取值范围为 0、1 或 2。

**由于它们没有 ctxSet，因此 ctxInc 直接等于 ctxIdx。然后，你可以将计算出的 ctxIdx 用于 CABAC 解码。**
