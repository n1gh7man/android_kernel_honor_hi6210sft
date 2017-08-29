/*************************************************************************
*   版权所有(C) 1987-2004, 深圳华为技术有限公司.
*
*   文 件 名 :  BSP_VIC.h
*
*   作    者 :  zhanghailun
*
*   描    述 :  中断模块用户接口文件
*
*   修改记录 :  2009年3月5日  v1.00  zhanghailun  创建
*************************************************************************/

#ifndef _BSP_VIC_H_
#define _BSP_VIC_H_

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
* 函 数 名  : BSP_INT_Enable
*
* 功能描述  : 使能某个中断
*
* 输入参数  : BSP_S32 s32Lvl 要使能的中断号，取值范围0～40  
* 输出参数  : 无
*
* 返 回 值  : OK&ERROR
*
* 修改记录  : 2009年3月5日   zhanghailun  creat
*****************************************************************************/


/*****************************************************************************
 * 函 数 名  : BSP_INT_Disable
 *
 * 功能描述  : 去使能某个中断
 *
 * 输入参数  : BSP_S32 s32Lvl 要使能的中断号，取值范围0～40  
 * 输出参数  : 无
 *
 * 返 回 值  : OK&ERROR
 *
 * 修改记录  : 2009年3月5日   zhanghailun  creat
 *****************************************************************************/


/*****************************************************************************
 * 函 数 名  : BSP_INT_Connect
 *
 * 功能描述  : 注册某个中断
 *
 * 输入参数  : VOIDFUNCPTR * vector 中断向量号，取值范围0～40
 *  		 VOIDFUNCPTR routine  中断服务程序
 *           BSP_S32 parameter      中断服务程序参数
 * 输出参数  : 无
 *
 * 返 回 值  : OK&ERROR
 *
 * 修改记录  : 2009年3月5日   zhanghailun  creat
 *****************************************************************************/

/*****************************************************************************
 * 函 数 名  : BSP_INT_DisConnect
 *
 * 功能描述  : 取消注册某个中断
 *
 * 输入参数  : VOIDFUNCPTR * vector 中断向量号，取值范围0～40
 *  		 VOIDFUNCPTR routine  中断服务程序
 *           BSP_S32 parameter      中断服务程序参数
 * 输出参数  : 无
 *
 * 返 回 值  : OK&ERROR
 *
 * 修改记录  : 2009年3月5日   zhanghailun  creat
 *****************************************************************************/

/*****************************************************************************
 * 函 数 名  : BSP_INT_Lock
 *
 * 功能描述  : 屏蔽所有中断
 *
 * 输入参数  : 无
 * 
 * 输出参数  : 无
 *
 * 返 回 值  : 中断锁号
 *
 * 修改记录  : 2009年3月5日   zhanghailun  creat
 *****************************************************************************/


/*****************************************************************************
 * 函 数 名  : BSP_INT_UnLock
 *
 * 功能描述  : 打开中断
 *
 * 输入参数  : BSP_S32 ulLocKKey  中断锁号，前一次BSP_INT_Lock的返回值
 * 
 * 输出参数  : 无
 *
 * 返 回 值  : 中断锁号
 *
 * 修改记录  : 2009年3月5日   zhanghailun  creat
 *****************************************************************************/


#ifdef __cplusplus
}
#endif

#endif /* end #define _BSP_VIC_H_*/

