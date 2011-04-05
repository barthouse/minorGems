#include "jri.h"
#include "minorGems/util/stringUtils.h"
#include "minorGems/util/SimpleVector.h"


#define JRI_VERSION  1


rgbaColor *extractJRI( unsigned char *inData, int inNumBytes,
                       int *outWidth, int *outHeight ) {
    

    }



unsigned char *generateJRI( rgbaColor *inRGBA, int inWidth, int inHeight,
                            int *outNumBytes ) {

    int numPixels = inWidth * inHeight;
        

    SimpleVector<rgbaColor> colors;
    char colorOverflow = false;
    
    unsigned char *pixelIndices = new unsigned char[ numPixels ];
    
    // build palette and indices into palette for each pixel
    for( int p=0; p<numPixels; p++ ) {
        unsigned char r, g, b;
        
        r = inRGBA[p].r;
        g = inRGBA[p].g;
        b = inRGBA[p].b;

        int foundIndex = -1;

        int numColors = colors.size();
        for( int c=0; c<numColors; c++ ) {
            rgbaColor color = *( colors.getElement( c ) );

            if( color.r == r && 
                color.g == g && 
                color.b == b ) {
                foundIndex = c;
                
                break;
                }
            }

        if( foundIndex != -1 ) {
            pixelIndices[p] = (unsigned char)foundIndex;
            }
        else {
            // new color
            if( colors.size() < 256 ) {
                colors.push_back( inRGBA[p] );
                
                pixelIndices[p] = colors.size() - 1;
                }
            else {
                // palette is full
                pixelIndices[p] = 0;
                colorOverflow = true;
                }
            }
        }        

    if( colorOverflow ) {
        delete [] pixelIndices;
        return NULL;
        }



    char *header = autoSprintf( "%d\n%d %d\n%d",
                                JRI_VERSION, 
                                inWidth, inHeight, 
                                colors.size() );
    

    SimpleVector<unsigned char> dataVector;
    

    dataVector.push_back( (unsigned char*)header, strlen( header ) );
    
    dataVector.push_back( '#' );

    // add palette bytes
    int numColors = colors.size();
    for( int c=0; c<numColors; c++ ) {
        rgbaColor color = *( colors.getElement( c ) );
        
        dataVector.push_back( color.r );
        dataVector.push_back( color.g );
        dataVector.push_back( color.b );
        }
    
    
    
    // run-length encode pixel index bytes

    SimpleVector<unsigned char> nonRunBytes;
    
    unsigned char currentRunByte = 0;
    
    unsigned char currentRunLength = 0;
    
    
    for( int p=0; p<numPixels; p++ ) {
        if( currentRunLength == 0 ) {
            currentRunByte = pixelIndices[p];
            currentRunLength ++;
            }
        else if( currentRunByte == pixelIndices[p] ) {
            if( currentRunLength < 255 ) {
                currentRunLength ++;
                }
            else {
                // run full, output it
                dataVector.push_back( 1 );
                dataVector.push_back( currentRunLength );
                dataVector.push_back( currentRunByte );

                // start a fresh run
                currentRunLength = 1;
                // keep current byte
                }
            }
        else if( currentRunLength < 3 ) {
            // a non-run that seemed like a run

            // add this non-run to our non-run byte vector
            for( int r=0; r<currentRunLength; r++ ) {
                
                if( nonRunBytes.size() == 255 ) {
                    // a full non-run
                    // output it
                    
                    dataVector.push_back( 0 );
                    dataVector.push_back( 255 );

                    for( int b=0; b<255; b++ ) {
                        
                        dataVector.push_back( 
                            *( nonRunBytes.getElement( b ) ) );
                        }
                    nonRunBytes.deleteAll();
                    }
                
                nonRunBytes.push_back( currentRunByte );
                }

            // see if a fresh run is starting
            currentRunByte = pixelIndices[p];
            currentRunLength = 1;
            }
        else {
            // current run of length 3 or greater
            // AND current run broken


            // first, output any non-run bytes that preceded the run

            int numNonRun = nonRunBytes.size();
            
            if( numNonRun > 0 ) {
            
                dataVector.push_back( 0 );
                dataVector.push_back( numNonRun );

                for( int b=0; b<numNonRun; b++ ) {
                        
                    dataVector.push_back( 
                        *( nonRunBytes.getElement( b ) ) );
                    }
                nonRunBytes.deleteAll();
                }
        
            // now output the run

            dataVector.push_back( 1 );
            dataVector.push_back( currentRunLength );
            dataVector.push_back( currentRunByte );
            
            // start a fresh run with this new, run-breaking byte
            currentRunLength = 1;
            currentRunByte = pixelIndices[p];
            }
        }
    
    delete [] pixelIndices;

            
    *outNumBytes = dataVector.size();
        
    return dataVector.getElementArray();
    }

