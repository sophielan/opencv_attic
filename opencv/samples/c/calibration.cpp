#include "cv.h"
#include "highgui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// example command line (for copy-n-paste):
// calibration -w 6 -h 8 -s 2 -n 10 -o camera.yml -op -oe [<list_of_views.txt>]

enum { DETECTION = 0, CAPTURING = 1, CALIBRATED = 2 };


int run_calibration( CvSeq* image_points_seq, CvSize img_size, CvSize board_size,
                     float square_size, float aspect_ratio, int flags,
                     CvMat* camera_matrix, CvMat* dist_coeffs, CvMat** extr_params )
{
    int code = 0;
    int image_count = image_points_seq->total;
    int point_count = board_size.width*board_size.height;
    CvMat* image_points = cvCreateMat( 1, image_count*point_count, CV_32FC2 );
    CvMat* object_points = cvCreateMat( 1, image_count*point_count, CV_32FC3 );
    CvMat* point_counts = cvCreateMat( 1, image_count, CV_32SC1 );
    CvMat rot_vects, trans_vects;
    int i, j, k;
    CvSeqReader reader;
    cvStartReadSeq( image_points_seq, &reader );

    // initialize arrays of points
    for( i = 0; i < image_count; i++ )
    {
        CvPoint2D32f* src_img_pt = (CvPoint2D32f*)reader.ptr;
        CvPoint2D32f* dst_img_pt = ((CvPoint2D32f*)image_points->data.fl) + i*point_count;
        CvPoint3D32f* obj_pt = ((CvPoint3D32f*)object_points->data.fl) + i*point_count;

        for( j = 0; j < board_size.height; j++ )
            for( k = 0; k < board_size.width; k++ )
            {
                *obj_pt++ = cvPoint3D32f(j*square_size, k*square_size, 0);
                *dst_img_pt++ = *src_img_pt++;
            }
        CV_NEXT_SEQ_ELEM( image_points_seq->elem_size, reader );
    }

    cvSet( point_counts, cvScalar(point_count) );

    *extr_params = cvCreateMat( image_count, 6, CV_32FC1 );
    cvGetCols( *extr_params, &rot_vects, 0, 3 );
    cvGetCols( *extr_params, &trans_vects, 3, 6 );

    cvZero( camera_matrix );
    cvZero( dist_coeffs );

    if( flags & CV_CALIB_FIX_ASPECT_RATIO )
    {
        camera_matrix->data.db[0] = aspect_ratio;
        camera_matrix->data.db[4] = 1.;
    }

    cvCalibrateCamera2( object_points, image_points, point_counts,
                        img_size, camera_matrix, dist_coeffs,
                        &rot_vects, &trans_vects, flags );

    cvReleaseMat( &object_points );
    cvReleaseMat( &image_points );
    cvReleaseMat( &point_counts );

    return code;
}


void save_camera_params( const char* out_filename, int image_count, CvSize img_size,
                         CvSize board_size, float square_size,
                         float aspect_ratio, int flags,
                         CvMat* camera_matrix, CvMat* dist_coeffs,
                         CvMat* extr_params, CvSeq* image_points_seq )
{
    CvFileStorage* fs = cvOpenFileStorage( out_filename, 0, CV_STORAGE_WRITE );
    
    time_t t;
    time( &t );
    struct tm *t2 = localtime( &t );
    char buf[1024];
    strftime( buf, sizeof(buf)-1, "%c", t2 );

    cvWriteString( fs, "calibration_time", buf );
    
    cvWriteInt( fs, "image_count", image_count );
    cvWriteInt( fs, "image_width", img_size.width );
    cvWriteInt( fs, "image_height", img_size.height );
    cvWriteInt( fs, "board_width", board_size.width );
    cvWriteInt( fs, "board_height", board_size.height );
    cvWriteReal( fs, "square_size", square_size );
    
    if( flags & CV_CALIB_FIX_ASPECT_RATIO )
        cvWriteReal( fs, "aspect_ratio", aspect_ratio );

    if( flags != 0 )
    {
        sprintf( buf, "flags: %s%s%s%s",
            flags & CV_CALIB_USE_INTRINSIC_GUESS ? "+use_intrinsic_guess" : "",
            flags & CV_CALIB_FIX_ASPECT_RATIO ? "+fix_aspect_ratio" : "",
            flags & CV_CALIB_FIX_PRINCIPAL_POINT ? "+fix_principal_point" : "",
            flags & CV_CALIB_ZERO_TANGENT_DIST ? "+zero_tangent_dist" : "" );
        cvWriteComment( fs, buf, 0 );
    }
    
    cvWriteInt( fs, "flags", flags );

    cvWrite( fs, "camera_matrix", camera_matrix );
    cvWrite( fs, "distortion_coefficients", dist_coeffs );

    if( extr_params )
    {
        cvWriteComment( fs, "a set of 6-tuples (rotatoin vector + translation vector) for each view", 0 );
        cvWrite( fs, "extrinsic_parameters", extr_params );
    }

    if( image_points_seq )
    {
        cvWriteComment( fs, "the array of board corners projections used for calibration", 0 );
        assert( image_points_seq->total == image_count );
        CvMat* image_points = cvCreateMat( 1, image_count*board_size.width*board_size.height, CV_32FC2 );
        cvCvtSeqToArray( image_points_seq, image_points->data.fl );

        cvWrite( fs, "image_points", image_points );
        cvReleaseMat( &image_points );
    }

    cvReleaseFileStorage( &fs );
}


int main( int argc, char** argv )
{
    CvSize board_size = {0,0};
    float square_size = 1.f, aspect_ratio = 1.f;
    const char* out_filename = "out_camera_data.yml";
    const char* input_filename = 0;
    int i, image_count = 0;
    int write_extrinsics = 0, write_points = 0;
    int flags = 0;
    CvCapture* capture = 0;
    FILE* f = 0;
    char imagename[1024];
    CvMemStorage* storage;
    CvSeq* image_points_seq = 0;
    int elem_size;
    int delay = 1000;
    clock_t prev_timestamp = 0;
    CvPoint2D32f* image_points_buf = 0;
    CvFont font = cvFont( 1, 1 );
    double _camera[9], _dist_coeffs[4];
    CvMat camera = cvMat( 3, 3, CV_64F, _camera );
    CvMat dist_coeffs = cvMat( 1, 4, CV_64F, _dist_coeffs );
    CvMat* extr_params = 0;
    int mode = DETECTION;
    int undistort_image = 0;
    const char* live_capture_help = 
        "When the live video from camera is used as input, the following hot-keys may be used:\n"
            "  <ESC>, 'q' - quit the program\n"
            "  'g' - start capturing images\n"
            "  'u' - switch undistortion on/off\n";

    if( argc < 2 )
    {
        printf( "This is a camera calibration sample.\n"
            "Usage: calibration\n"
            "     -w <board_width>         # the number of inner corners per one of board dimension\n"
            "     -h <board_height>        # the number of inner corners per another board dimension\n"
            "     [-n <number_of_frames>]  # the number of frames to use for calibration\n"
            "                              # (if not specified, it will be set to the number\n"
            "                              #  of board views actually available)\n"
            "     [-d <delay>]             # a minimum delay in ms between subsequent attempts to capture a next view\n"
            "                              # (used only for video capturing)\n"
            "     [-s <square_size>]       # square size in some user-defined units (1 by default)\n"
            "     [-o <out_camera_params>] # the output filename for intrinsic [and extrinsic] parameters\n"
            "     [-op]                    # write points\n"
            "     [-oe]                    # write extrinsic parameters\n"
            "     [-zt]                    # assume zero tangential distortion\n"
            "     [-a <aspect_ratio>]      # fix aspect ratio (fx/fy)\n"
            "     [-p]                     # fix the principal point at the center\n"
            "     [input_data]             # input data, one of the following:\n"
            "                              #  - text file with a list of the images of the board\n"
            "                              #  - name of video file with a video of the board\n"
            "                              # if it is not specified, a live view from the camera is used\n"
            "\n" );
        printf( "%s", live_capture_help );
        return 0;
    }

    for( i = 1; i < argc; i++ )
    {
        const char* s = argv[i];
        if( strcmp( s, "-w" ) == 0 )
        {
            if( sscanf( argv[++i], "%u", &board_size.width ) != 1 || board_size.width <= 0 )
                return fprintf( stderr, "Invalid board width\n" ), -1;
        }
        else if( strcmp( s, "-h" ) == 0 )
        {
            if( sscanf( argv[++i], "%u", &board_size.height ) != 1 || board_size.height <= 0 )
                return fprintf( stderr, "Invalid board height\n" ), -1;
        }
        else if( strcmp( s, "-s" ) == 0 )
        {
            if( sscanf( argv[++i], "%f", &square_size ) != 1 || square_size <= 0 )
                return fprintf( stderr, "Invalid board square width\n" ), -1;
        }
        else if( strcmp( s, "-n" ) == 0 )
        {
            if( sscanf( argv[++i], "%u", &image_count ) != 1 || image_count <= 3 )
                return printf("Invalid number of images\n" ), -1;
        }
        else if( strcmp( s, "-a" ) == 0 )
        {
            if( sscanf( argv[++i], "%f", &aspect_ratio ) != 1 || aspect_ratio <= 0 )
                return printf("Invalid aspect ratio\n" ), -1;
        }
        else if( strcmp( s, "-d" ) == 0 )
        {
            if( sscanf( argv[++i], "%u", &delay ) != 1 || delay <= 0 )
                return printf("Invalid delay\n" ), -1;
        }
        else if( strcmp( s, "-op" ) == 0 )
        {
            write_points = 1;
        }
        else if( strcmp( s, "-oe" ) == 0 )
        {
            write_extrinsics = 1;
        }
        else if( strcmp( s, "-zt" ) == 0 )
        {
            flags |= CV_CALIB_ZERO_TANGENT_DIST;
        }
        else if( strcmp( s, "-p" ) == 0 )
        {
            flags |= CV_CALIB_FIX_PRINCIPAL_POINT;
        }
        else if( strcmp( s, "-o" ) == 0 )
        {
            out_filename = argv[++i];
        }
        else if( s[0] != '-' )
            input_filename = s;
        else
            return fprintf( stderr, "Unknown option %s", s ), -1;
    }

    if( input_filename )
    {
        capture = cvCreateFileCapture( input_filename );
        if( !capture )
        {
            f = fopen( input_filename, "rt" );
            if( !f )
                return fprintf( stderr, "The input file could not be opened\n" ), -1;
            image_count = 0;
        }
        mode = CAPTURING;
    }
    else
        capture = cvCreateCameraCapture(0);

    if( !capture && !f )
        return fprintf( stderr, "Could not initialize video capture\n" ), -2;

    if( capture )
        printf( "%s", live_capture_help );

    elem_size = board_size.width*board_size.height*sizeof(image_points_buf[0]);
    storage = cvCreateMemStorage( MAX( elem_size*4, 1 << 16 ));
    image_points_buf = (CvPoint2D32f*)cvAlloc( elem_size );

    cvNamedWindow( "Image View", 1 );

    for(;;)
    {
        IplImage *view = 0, *view_gray = 0;
        int count = 0, found, blink = 0;
        CvPoint text_origin;
        CvSize text_size = {0,0};
        int base_line = 0;
        char s[100];
        int key;
        
        if( f )
        {
            fgets( imagename, sizeof(imagename)-2, f );
            view = cvLoadImage( imagename, 1 );
        }
        else
        {
            IplImage* view0 = cvQueryFrame( capture );
            if( view0 )
                view = cvCloneImage( view0 );
        }

        if( !view )
            break;

        found = cvFindChessboardCorners( view, board_size,
            image_points_buf, &count, CV_CALIB_CB_ADAPTIVE_THRESH );

#if 1
        // improve the found corners' coordinate accuracy
        view_gray = cvCreateImage( cvGetSize(view), 8, 1 );
        cvCvtColor( view, view_gray, CV_BGR2GRAY );
        cvFindCornerSubPix( view_gray, image_points_buf, count, cvSize(11,11),
            cvSize(-1,-1), cvTermCriteria( CV_TERMCRIT_EPS+CV_TERMCRIT_ITER, 30, 0.1 ));
        cvReleaseImage( &view_gray );
#endif

        if( mode == CAPTURING && found && (f || clock() - prev_timestamp > delay*1e-3*CLOCKS_PER_SEC) )
        {
            cvSeqPush( image_points_seq, image_points_buf );
            prev_timestamp = clock();
            blink = !f;
        }

        cvDrawChessboardCorners( view, board_size, image_points_buf, count, found );

        cvGetTextSize( "100/100", &font, &text_size, &base_line );
        text_origin.x = view->width - text_size.width - 10;
        text_origin.y = view->height - base_line - 10;

        if( image_count > 0 )
            sprintf( s, "%d/%d", image_points_seq ? image_points_seq->total : 0, image_count );
        else
            sprintf( s, "%d/?", image_points_seq ? image_points_seq->total : 0 );

        cvPutText( view, s, text_origin, &font, CV_RGB(255,0,0) );

        if( blink )
            cvNot( view, view );

        if( mode == CALIBRATED && undistort_image )
        {
            IplImage* t = cvCloneImage( view );
            cvUndistort2( t, view, &camera, &dist_coeffs );
            cvReleaseImage( &t );
        }

        cvShowImage( "Image View", view );
        key = cvWaitKey(50);
        if( key == 27 )
            break;
        
        if( key == 'u' && mode == CALIBRATED )
            undistort_image = !undistort_image;

        if( key == 'g' )
        {
            mode = CAPTURING;
            cvClearMemStorage( storage );
            image_points_seq = cvCreateSeq( 0, sizeof(CvSeq), elem_size, storage );
        }

        if( mode == CAPTURING && image_points_seq->total >= image_count )
        {
            cvReleaseMat( &extr_params );
            int code = run_calibration( image_points_seq, cvGetSize(view), board_size,
                square_size, aspect_ratio, flags, &camera, &dist_coeffs, &extr_params );
            if( code >= 0 )
            {
                save_camera_params( out_filename, image_count, cvGetSize(view),
                    board_size, square_size, aspect_ratio, flags,
                    &camera, &dist_coeffs, write_extrinsics ? extr_params : 0,
                    write_points ? image_points_seq : 0 );
                mode = CALIBRATED;
            }
            else
                mode = DETECTION;
        }

        cvReleaseImage( &view );
    }

    if( capture )
        cvReleaseCapture( &capture );
    return 0;
}
